#include "datacenter_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <errno.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}

	return NULL;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <math.h>

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	pthread_mutex_lock(&dc->lock);

	Resources total_resourcers[dc->num_servers];
	size_t count_hosted_vms[dc->num_servers];
	memset(total_resourcers, 0, sizeof(total_resourcers));
	memset(count_hosted_vms, 0, sizeof(count_hosted_vms));

	for(size_t i = 0; i < dc->num_servers; i++) {
		Server *server = &dc->servers[i];
		total_resourcers[i] = server->total;
		count_hosted_vms[i] = 0;
	}

	size_t total_vms = 0;
	for (size_t i = 0; i < reservation->items_count; i++) {
		size_t amount = reservation->items[i].amount;

        if (amount == 0 || amount > MAX_RESERVATION_VMS - total_vms) {
			fprintf(stderr, "Reservation exceeds VM limit or has zero VMs.\n");
			pthread_mutex_unlock(&dc->lock);
			return 1;
    }

		total_vms += amount;

		ReservationItem *item = &reservation->items[i];
		VMType *vm_type = VMType_exists(dc, item->vm_type_id);
		size_t server_idx = item->server_id-1;

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
        	pthread_mutex_unlock(&dc->lock);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
        	pthread_mutex_unlock(&dc->lock);
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if (!resources_can_fit(total_resourcers[server_idx], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough total resources.\n", item->server_id);
				pthread_mutex_unlock(&dc->lock);
				return 1;
			}

			resources_sub(&total_resourcers[server_idx], vm_type->required);

			count_hosted_vms[server_idx]++;
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	for(size_t i = 0; i < dc->num_servers; i++) {
		if(count_hosted_vms[i] > MAX_HOSTED_VMS) {
			fprintf(stderr, "Reservation exceeds VM limit\n");
			pthread_mutex_unlock(&dc->lock);
			return 1;
		}
	}
	pthread_mutex_unlock(&dc->lock);

	return 0;
}

/* 
 * Must hold reservation_lock and all corresponding server locks before calling 
 *
*/
static int reservation_apply(DataCenter *dc, Reservation *reservation){
	VM ** new_vms;
	size_t vm_count = 0;
	for (size_t i = 0; i < reservation->items_count; i++) {
		vm_count += reservation->items[i].amount;
	}

	new_vms = calloc(vm_count, sizeof(VM *));

	for (size_t i = 0; i < vm_count; i++) {
		new_vms[i] = calloc(1, sizeof(VM));
		if (new_vms[i] == NULL) { 
			fprintf(stderr, "Failed to allocate VM.\n");
			return 1;
		}
	}

	vm_count = 0;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = new_vms[vm_count++];

			snprintf(new_vm->id,
                MAX_VM_ID_STRING,
                "%.*s_%zu",
                (int)(MAX_VM_ID_STRING - 22), // 22 = "_<index>" + null terminator
                reservation->id,
                reservation->num_vms + 1
            );

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	free(new_vms);

	return 0;
}

int reservation_try_apply(DataCenter *dc, Reservation *reservation) {
	Resources available_resourcers[dc->num_servers];
	size_t saved_num_hosted_vms[dc->num_servers];
	memset(available_resourcers, 0, sizeof(available_resourcers));
	memset(saved_num_hosted_vms, 0, sizeof(saved_num_hosted_vms));

	pthread_mutex_lock(&dc->lock);

	if (reservation_exists(dc, reservation->id)) {
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		pthread_mutex_unlock(&dc->lock);
		return 1;
	}

	if (dc->num_reservations >= MAX_RESERVATIONS) {
		pthread_mutex_unlock(&dc->lock);
		return 2;
	}

	for(size_t i = 0; i < dc->num_servers; i++) {
		Server *server = &dc->servers[i];

		available_resourcers[i] = server->available;
		saved_num_hosted_vms[i] = server->num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		VMType *vm_type = item->vm_type;
		size_t server_idx = item->server_id-1;

		if ((saved_num_hosted_vms[server_idx] + item->amount) >= MAX_HOSTED_VMS) {
			pthread_mutex_unlock(&dc->lock);
			return 1;
	  	}

		for (size_t j = 0; j < item->amount; j++) {
			if (!resources_can_fit(available_resourcers[server_idx], vm_type->required)) {
				pthread_mutex_unlock(&dc->lock);
				return 1;
			}

			resources_sub(&available_resourcers[server_idx], vm_type->required);
			saved_num_hosted_vms[server_idx]++;
		}
	}

	reservation_apply(dc, reservation);
	  
  	dc->reservations[dc->num_reservations++] = *reservation;
	
	pthread_mutex_unlock(&dc->lock);

	return 0;
}


Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

void vm_destroy(DataCenter *dc, pid_t term_vm) {
	VM * vm_to_destroy = NULL;

	pthread_mutex_lock(&dc->lock);

	for(size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *reservation = &dc->reservations[i];

		for(size_t k = 0; k < reservation->num_vms; k++) {
			VM *vm = reservation->vms[k];

			if(vm == NULL || vm->pid != term_vm)
				continue;

			vm_to_destroy = vm;

			char base_dir[MAX_PATH_SIZE];
			char vm_dir[MAX_PATH_SIZE];

			if (cloudist_base_dir(base_dir, sizeof(base_dir)) == 0) {
				int written = snprintf(vm_dir, sizeof(vm_dir), "%s/%s/%s",
				                       base_dir, reservation->id, vm->id);

				if (written > 0 && (size_t)written < sizeof(vm_dir))
					remove_dir_recursive(vm_dir);
			}

			reservation->vms[k] = NULL;
			reservation->running_vms--;

			if(reservation->running_vms == 0)
				reservation_destroy(dc, reservation);
		}
	}

	if (vm_to_destroy == NULL) {
		pthread_mutex_unlock(&dc->lock);
		return;
	}

	Server *server = vm_to_destroy->server;

	// Return the VM resources back to the hosting server.
	resources_add(&server->available, vm_to_destroy->type->required);

	// Remove the VM from the server's hosted VM list.
	for (size_t j = 0; j < server->num_hosted_vms; j++) {
		if (server->hosted_vms[j] == vm_to_destroy) {
			// Shift the remaining VMs one position to the left.
			memmove(&server->hosted_vms[j],
							&server->hosted_vms[j + 1],
							(server->num_hosted_vms - j - 1) * sizeof(VM *));

			server->num_hosted_vms--;
			break;
		}
	}

	pthread_mutex_unlock(&dc->lock);

	free(vm_to_destroy);
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	if(reservation->running_vms != 0)
		return;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	char base_dir[MAX_PATH_SIZE];
	char res_dir[MAX_PATH_SIZE];

	if (cloudist_base_dir(base_dir, sizeof(base_dir)) == 0) {
		int written = snprintf(res_dir, sizeof(res_dir), "%s/%s",
		                       base_dir, reservation->id);

		if (written > 0 && (size_t)written < sizeof(res_dir))
			rmdir(res_dir);
	}

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}

// Creates a directory, ignoring the case where it already exists.
static int make_dir(const char *path) {
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "Failed to create directory %s.\n", path);
		return 1;
	}

	return 0;
}

int vm_prepare_directory(const char *reservation_id, VM *vm, char *vm_dir, size_t size) {
	char base_dir[MAX_PATH_SIZE];
	char res_dir[MAX_PATH_SIZE];
	char input_folder[MAX_PATH_SIZE];

	if (cloudist_base_dir(base_dir, sizeof(base_dir)) != 0)
		return 1;

	int res_written = snprintf(res_dir, sizeof(res_dir), "%s/%s",
	                           base_dir, reservation_id);

	int vm_written = snprintf(vm_dir, size, "%s/%s/%s",
	                          base_dir, reservation_id, vm->id);
	if (res_written < 0 || (size_t)res_written >= sizeof(res_dir) ||
	    vm_written < 0 || (size_t)vm_written >= size) {
		fprintf(stderr, "VM directory path too long for VM %s.\n", vm->id);
		return 1;
	}

	if (absolute_path(vm->type->input_folder, input_folder, MAX_PATH_SIZE) != 0) {
		fprintf(stderr, "Failure to retrieve input folder's absolute path\n");
		return 1;
	}

	// copy_dir_recursive only creates the last component, so the two parents
	// have to exist first.
	if (make_dir(base_dir) != 0 || make_dir(res_dir) != 0)
		return 1;

	// Replicates the whole input tree into the VM's own folder.
	if (copy_dir_recursive(input_folder, vm_dir) != 0) {
		fprintf(stderr, "Failed to replicate inputs for VM %s.\n", vm->id);
		return 1;
	}

	return 0;
}

// CPULIMIT = 100 * VM vCPUs * local cores selected for CloudIST
//            / (vCPUs per server * number of servers)
// May exceed 100; its maximum is 100 * local cores.
static long vm_cpu_limit(DataCenter *dc, VMType *type) {
	double limit = 100.0 * type->required.cpu * (double) dc->local_cores
	               / (dc->servers[0].total.cpu * (double) dc->num_servers);
	long max_limit = 100 * (long) dc->local_cores;
	long rounded = lround(limit);

	if (rounded < 1)
		return 1;

	if (rounded > max_limit)
		return max_limit;

	return rounded;
}

void spawn_vm_child(DataCenter *dc, VM *vm, const char *vm_dir) {
	// RAM
	struct rlimit mem_limit;
	rlim_t ram_bytes = (rlim_t)vm->type->required.ram * 1024 * 1024 * 1024;
	mem_limit.rlim_cur = ram_bytes;
	mem_limit.rlim_max = ram_bytes;
	if (setrlimit(RLIMIT_AS, &mem_limit) != 0) {
		perror("Failed to set RAM limit");
	}

	char cpu_limit_str[32];
	snprintf(cpu_limit_str, sizeof(cpu_limit_str), "%ld", vm_cpu_limit(dc, vm->type));

	char exec_path[MAX_PATH_SIZE];

	if (absolute_path(vm->type->exec_path, exec_path, MAX_PATH_SIZE) != 0) {
	    fprintf(stderr, "Failure to retrieve executable's absolute path\n");
		_exit(1);
	}

	if (chdir(vm_dir) != 0) {
        perror("Failure to change directory to VM folder");
        _exit(1);
    }

	int null_fd = open("/dev/null", O_WRONLY);
	if(null_fd == -1) {
		fprintf(stderr, "Failure to open /dev/null\n");
		_exit(1);
	}

	close(STDOUT_FILENO);
	if(dup2(null_fd, STDOUT_FILENO) == -1) {
		fprintf(stderr, "Failure to redirect child stdout to /dev/null\n");
		_exit(1);
	}
	close(null_fd);

	execlp("cpulimit",
					"cpulimit",
					"-q",                 // Mute messages like "process XXXX detected/dead!".
					"-f",                 // Makes cpu limit run on the foreground.
					"-l", cpu_limit_str,
					"--",
					exec_path,
					NULL);
	// failed to exec
	fprintf(stderr, "Failed to exec for VM %s.\n", vm->id);
	_exit(1);
}

int spawn_all_vms(DataCenter *dc, Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];

		char vm_dir[MAX_PATH_SIZE];

		if (vm_prepare_directory(res->id, vm, vm_dir, sizeof(vm_dir)) != 0) {
				fprintf(stderr, "Failed to prepare directory for VM %s.\n", vm->id);
				return 1;
		}

		pid_t pid = fork();
		if (pid < 0) {
				fprintf(stderr, "Failed to fork for VM %s.\n", vm->id);
				return 1;
		} else if (pid == 0) {
				spawn_vm_child(dc, vm, vm_dir); // does not return
		} else {
				vm->pid = pid;
				vm->state = VM_STATE_RUNNING;
		}
	}

	return 0;
}

void wait_for_all_vms(DataCenter *dc, Reservation *reservation) {
	size_t num_vms = reservation->num_vms;
	size_t running_vms = reservation->running_vms;
	for (size_t i = 0; i < num_vms && running_vms > 0; i++) {
		VM *vm = reservation->vms[i];
		if (vm == NULL || vm->state != VM_STATE_RUNNING)
			continue;

		pid_t vm_pid = vm->pid;
		int status;
		pid_t result;
		result = waitpid(vm_pid, &status, 0);

		if (result == -1) {
			perror("waitpid failed");
			continue;
		}

		vm_destroy(dc, vm_pid);
		running_vms--;
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}