#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "parser.h"


static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

void datacenter_printf(const char *format, ...) {
  va_list args;

  pthread_mutex_lock(&print_lock);

  va_start(args, format);
  vprintf(format, args);
  va_end(args);

  fflush(stdout);

  pthread_mutex_unlock(&print_lock);
}

void * datacenter_vm_wait_thread(void *arg) {
  DataCenter *dc = (DataCenter *) arg;

  while (1) {
    // Read running under the same lock used when shutdown changes it.
    pthread_mutex_lock(&dc->lock);
    int should_stop = !dc->running;
    pthread_mutex_unlock(&dc->lock);

    if (should_stop)
      break;

    int wstatus = 0;
    pid_t term_pid = waitpid(-1, &wstatus, 0);

    if (term_pid == -1 && errno == EINTR)
      continue;

    if (term_pid > 0) {
      vm_destroy(dc, term_pid);
    } else if (errno == ECHILD) {
      // No child is available; pause to avoid spinning while the waiter is idle.
      // There's a better way to do this, but it requires more complex synchronization.
      sleep(1);
    } else {
      perror("Failed to wait for VM");
      sleep(1);
    }
  }

  // Go backwards through the reservations to wait for all VMs to finish, since
  // removing reservations from the front of the list will shift the remaining reservations down.
  size_t i = dc->num_reservations;
  while (i > 0) {
    i--;
    wait_for_all_vms(dc, &dc->reservations[i]);
  }

  return NULL;
}

int datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;
  dc->num_vm_types = 0;
  dc->num_reservations = 0;

  int init_error = pthread_mutex_init(&dc->lock, NULL);
  if (init_error != 0) {
    fprintf(stderr, "Failed to initialize Data Center mutex: %s\n",
            strerror(init_error));
    return 1;
  }

  dc->running = 1;
  init_error = pthread_create(&dc->waiter_tid, NULL, datacenter_vm_wait_thread, (void*) dc);
  if (init_error != 0) {
    fprintf(stderr, "Failed to create VM waiting thread: %s\n",
            strerror(init_error));
    pthread_mutex_destroy(&dc->lock);
    return 1;
  }

  return 0;
}

void datacenter_destroy(DataCenter *dc) {
  // Set the stop flag under the lock so the waiter sees it consistently.
  pthread_mutex_lock(&dc->lock);
  dc->running = 0;
  pthread_mutex_unlock(&dc->lock);

  pthread_join(dc->waiter_tid, NULL);

  for (size_t i = 0; i < dc->num_servers; i++) {
    for(size_t j = 0; j < dc->servers[i].num_hosted_vms; j++){
      free(dc->servers[i].hosted_vms[j]);
    }
  }
  free(dc->servers);
  pthread_mutex_destroy(&dc->lock);
}

static int process_config_file(DataCenter *dc, char * config_path) {
  int config_fd = open(config_path, O_RDONLY);
  if(config_fd == -1) {
    fprintf(stderr, "Cannot open %s config file\n", config_path);
    return 1;
  }

  int is_reservation_pending = 0;
  
  while(1) {
    switch (get_next_command(config_fd)) {
      case CMD_DEFINE: {
          VMType vmtype;

          if (parse_define(config_fd, &vmtype) != 0) {
              fprintf(stderr, "Invalid define command.\n");
              continue;
          }

          if(datacenter_define_VM(dc, &vmtype) != 0){
              fprintf(stderr, "Failed to define VM.\n");
              return 1;
          }

          datacenter_printf("VM successfully defined!\n");

          break;
      }

      case CMD_RESERVE: {
          Reservation reservation = {0};

          size_t num_items = parse_reserve(config_fd, &reservation, MAX_RESERVATIONS_ITEMS);

          if(is_reservation_pending) {
              fprintf(stderr, "Only one reserveation allowed per .conf file.\n");
              continue;
          }

          if (num_items == 0) {
              fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
              continue;
          }

          if (datacenter_reserve(dc, &reservation) != 0) {
              fprintf(stderr, "Failed to reserve VMs.\n");
              return 1;
          }

          is_reservation_pending = 1;

          datacenter_printf("Reservation made successfully!\n");

          break;
      }

      case CMD_EXECUTE:
        char id[MAX_STRING_SIZE];

        if(!is_reservation_pending) {
              fprintf(stderr, "No reservation pending execution.\n");
              continue;
        }

        if(parse_execute(config_fd, id) != 0){
            fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
            continue;
        }

        if (datacenter_execute(dc, id) != 0) {
            fprintf(stderr, "Failed to execute reservation.\n");
            continue;
        }

        datacenter_printf("Finished reservation execution!\n");

        break;

      case CMD_LIST:
          if (datacenter_list(dc) != 0) {
              fprintf(stderr, "Failed to list VMs.\n");
              continue;
          }

          break;

      case CMD_WAIT:
          unsigned int delay;

          if(parse_wait(config_fd, &delay) != 0){
              fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
              continue;
          }

          datacenter_wait(delay);
          break;

      case CMD_INVALID:
          fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
          break;

      case EOC:
        return 0;

      case CMD_EMPTY:
        continue;

      default: 
          datacenter_printf("Invalid command.\n");
          return 1;
    }
  }
}

struct config_queue {
    DataCenter * dc;
    const char * input_dir;
    struct dirent ** entries;
    int num_entries;
    int next;
    pthread_mutex_t lock;
};

static void * datacenter_config_consumer_thread(void * arg) {
  struct config_queue * queue = (struct config_queue*) arg;
  size_t input_f_size = strlen(queue->input_dir);

  while(1) {
    pthread_mutex_lock(&queue->lock);
    if(queue->next >= queue->num_entries) {
      pthread_mutex_unlock(&queue->lock);
      break;
    }
    int i = queue->next++;
    pthread_mutex_unlock(&queue->lock);

    size_t f_size = strlen(queue->entries[i]->d_name);
    size_t filename_size = input_f_size + 1 + f_size + 1;
    char *config_path = malloc(filename_size);

    snprintf(config_path, filename_size, "%s/%s", queue->input_dir, queue->entries[i]->d_name);

    if(process_config_file(queue->dc, config_path)) {
      fprintf(stderr, "Invalid config file %s!\n", config_path);
    }

    free(config_path);
  }

  return NULL;
}

static int is_config_file(const struct dirent *dent) {
    size_t f_size = strlen(dent->d_name);

    if(f_size < 5)
        return 0;

    return strcmp(dent->d_name + (f_size-5), ".conf") == 0;
}

static int datacenter_process_all_config_files(DataCenter * dc, char * input_dir, size_t num_workers) {
    struct dirent **entries;
    int ret = 0;

    int num_entries = scandir(input_dir, &entries, is_config_file, alphasort);

    if(num_entries < 0) {
        fprintf(stderr, "Invalid input directory.\n");
        return 1;
    }

    struct config_queue queue = {
        .dc = dc,
        .input_dir = input_dir,
        .entries = entries,
        .num_entries = num_entries,
        .next = 0,
    };
    pthread_mutex_init(&queue.lock, NULL);

    if(num_workers > (size_t) num_entries)
        num_workers = (size_t) num_entries;

    pthread_t pids[num_workers > 0 ? num_workers : 1];
    size_t num_threads = 0;

    for(; num_threads < num_workers; num_threads++) {
        if(pthread_create(pids+num_threads, NULL, datacenter_config_consumer_thread, (void*) &queue)) {
          fprintf(stderr, "Failed to create config parsing threads.\n");
          ret = 1;
          break;
        }
    }

    if(num_threads == 0)
        datacenter_config_consumer_thread(&queue);

    for(size_t j = 0; j < num_threads; j++) {
        pthread_join(pids[j], NULL);
    }

    pthread_mutex_destroy(&queue.lock);

    for (int i = 0; i < num_entries; i++) {
        free(entries[i]);
    }
    free(entries);

    return ret;
}

int datacenter_run(DataCenter *dc, size_t num_servers, Resources *resources, char * input_dir, size_t num_workers, size_t local_cores) {
  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
  }

  dc->num_servers = num_servers;
  dc->local_cores = local_cores;

  if(datacenter_process_all_config_files(dc, input_dir, num_workers)) {
    fprintf(stderr, "Failed to parse all config files.\n");
    return 1;
  }

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
    if (!file_exists(type->exec_path)) {
        fprintf(stderr, "Invalid executable path %s.\n", type->exec_path);
        return 1;
    }

    if (!path_exists(type->input_folder)) {
        fprintf(stderr, "Invalid input folder %s.\n", type->input_folder);
        return 1;
    }

    pthread_mutex_lock(&dc->lock);
    if(VMType_exists(dc, type->id) != NULL){
        pthread_mutex_unlock(&dc->lock);
        fprintf(stderr, "VM type with id %s already exists.\n", type->id);
        return 1;
    }

    if(dc->num_vm_types >= MAX_VM_TYPES){
        pthread_mutex_unlock(&dc->lock);
        fprintf(stderr, "Maximum of VM types reached.\n");
        return 1;
    }

    dc->vm_types[dc->num_vm_types++] = *type;

    pthread_mutex_unlock(&dc->lock);

    return 0;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
    if(reservation_validate(dc, reservation)) {
        return 1;
    }

    while(1) {
        switch(reservation_try_apply(dc, reservation)) {
            case 0: // success
                return 0;

            case 2: // failure, don't try again
                return 1;

            case 1: // failure, resources in used

                fprintf(stderr, "No enough current capacity. Waiting...\n");
                // Wait for 1 second before trying again 
                //(could be improved with a condition variable to avoid busy waiting)
                sleep(1); 
                continue;

            default:
                return 1;
        }
    }  
}

int datacenter_execute(DataCenter *dc, const char *reservation_id) {
    pthread_mutex_lock(&dc->lock);
    Reservation *res = find_pending_reservation(dc, reservation_id);

    if (!res) {
        pthread_mutex_unlock(&dc->lock);
        return 1;
    }

    // Must be set before spawn_all_vms, otherwise might race with the waiting thread
    res->running_vms = res->num_vms;
    if (spawn_all_vms(dc, res) != 0) {
        pthread_mutex_unlock(&dc->lock);
        return 1;
    }

    res->state = RES_STATE_RUNNING;

    pthread_mutex_unlock(&dc->lock);

    return 0;
}

int datacenter_list(DataCenter *dc) {
  pthread_mutex_lock(&dc->lock);

  pthread_mutex_lock(&print_lock);

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server *server = &dc->servers[i];

    Resources used = server->total;
    resources_sub(&used, server->available);

    printf(" Server #%zu\n", server->id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server->total.cpu, server->total.ram, server->total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server->available.cpu, server->available.ram, server->available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server->num_hosted_vms);

    if (server->num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server->num_hosted_vms; j++) {
        VM *vm = server->hosted_vms[j];
        const char *prefix = (j == server->num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");
  
  fflush(stdout);
  pthread_mutex_unlock(&print_lock);

  pthread_mutex_unlock(&dc->lock);

  return 0;
}

void datacenter_wait(unsigned int delay_ms){
  struct timespec delay = (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep(&delay, NULL);
}