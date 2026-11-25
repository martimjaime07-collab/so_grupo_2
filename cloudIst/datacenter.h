#ifndef DATACENTER__H
#define DATACENTER__H

#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

#include "resources.h"
#include "../common/constants.h"
#include "../common/filesystem.h"
#include "../common/common.h"


typedef struct DataCenter {
    Server *servers;                 // List of Servers.
    size_t num_servers;              // Number of Servers.

    pthread_mutex_t lock;     // Protects shared DataCenter state, including running.

    VMType vm_types[MAX_VM_TYPES];
    size_t num_vm_types;

    Reservation reservations[MAX_RESERVATIONS];
    size_t num_reservations;

    int running;                 
    pthread_t waiter_tid;

    size_t local_cores;
} DataCenter;
/**
 * Thread-safe printf: prints to stdout while holding the print lock
 * and flushes stdout before returning.
 *
 * Must not be called by a child process after fork().
 *
 * @param format printf-style format string.
 */
void datacenter_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

/**
 * Initializes a Data Center.
 *
 * @param dc DataCenter being initialized.
 * 
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_init(DataCenter *dc);

/**
 * Stops the VM waiter, waits for running VMs to finish, and frees the
 * Data Center's allocated resources.
 *
 * @param dc DatacCenter being destroyed.
 */
void datacenter_destroy(DataCenter *dc);

/**
 * Configures a given Data Center.
 *
 * @param dc Pointer to Datacenter being configured.
 * @param resources Pointer to resources of every PC on the Datacenter.
 * @param input_path VM and Reserve configuration file
 * @param num_workers Maximum number of config files processed concurrently.
 * @param local_cores Number of local machine cores selected for CloudIST.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_run(DataCenter *dc, size_t num_servers, Resources *resources, char * input_dir, size_t num_workers, size_t local_cores);

/**
 * Defines a VM for the Data Center
 *
 * @param dc Pointer to a Data Center.
 * @param type Pointer to VM type.
 * 
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_define_VM(DataCenter *dc, struct VMType *type);

/**
 * Reserves a set of VMs for a given reservation.
 *
 * @param dc Pointer to a Data Center.
 * @param reservation Pointer to a Reservation.
 * 
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_reserve(DataCenter *dc, Reservation *reservation);

/**
 * Lists the current state of the Data Center.
 *
 * @param dc Pointer to a Data Center.
 * 
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_list(DataCenter *dc);

/**
 * Executes a reservation, launching the VMs.
 *
 * @param dc Pointer to a Data Center.
 * @param reservation_id ID of the reservation to execute.
 * 
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_execute(DataCenter *dc, const char *reservation_id);

/**
 * Executes a delay command.
 * 
 * @param delay_ms Delay in ms.
 */
void datacenter_wait(unsigned int delay_ms);

#endif // DATACENTER__H