#ifndef DATACENTER_UTILS__H
#define DATACENTER_UTILS__H

#include "datacenter.h"

/**
 * Checks if a VMType with the given id exists.
 * 
 * @param dc Pointer to Data Center.
 * @param type_id Id of VM type being checked.
 * 
 * @return Pointer to VMType if it exists.
 * @return NULL if VMType does not exist.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id);

/**
 * Simulates the placement of every VM requested in the reservation using a
 * temporary copy of each server's available resources. The Data Center state
 * is left unchanged regardless of the outcome.
 *
 * Validation fails if any requested server does not have enough available
 * resources to host all the requested VMs assigned to it or if the request 
 * does not have valid parameters (VM types that do not exist, invalid server ids).
 *
 * @param dc Data Center where the reservation will be validated.
 * @param reservation Reservation to validate.
 *
 * @return 0 if the reservation can be fully allocated.
 * @return 1 otherwise.
 */
int reservation_validate(DataCenter *dc, Reservation *reservation);

int reservation_try_apply(DataCenter *dc, Reservation *reservation);

/**
 * Looks up a reservation by id and checks that it is pending.
 *
 * @param dc Pointer to a Data Center.
 * @param reservation_id ID of the reservation to look up.
 *
 * @return Pointer to the Reservation if found and RES_STATE_PENDING.
 * @return NULL if not found or not in RES_STATE_PENDING (error is printed).
 */
Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id);

/**
 * Frees a VM, returns their resources to the corresponding hosting servers,
 * removes it from each server's hosted VM list.
 * 
 * @param dc Pointer to the Data Center containing the reservation.
 * @param term_vm pid of the terminating VM.
 */
void vm_destroy(DataCenter *dc, pid_t term_vm);

/**
 * Removes finished reservation from the Data Center reservation
 * list. If called with a reservation with VMs in progress, nothing happens
 *
 * @param dc Pointer to the Data Center containing the reservation.
 * @param reservation Pointer to the reservation to destroy.
 */
void reservation_destroy(DataCenter *dc, Reservation *reservation);

/**
 * Builds the VM directory path (/tmp/CloudIST/<reserva>/<vm>), creates it
 * along with any missing parent, and recursively replicates the contents of
 * the VM type's input directory into it.
 *
 * Runs in the parent, before fork(), so that each VM gets its own private
 * copy of the inputs even when several VMs share the same VM type.
 *
 * @param reservation_id Id of the reservation owning the VM.
 * @param vm Pointer to the VM whose directory is being prepared.
 * @param vm_dir Buffer that receives the resulting directory path.
 * @param size Size of the vm_dir buffer, in bytes.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int vm_prepare_directory(const char *reservation_id, VM *vm, char *vm_dir, size_t size);

/**
 * Runs in the child process after fork(): sets resource limits (RAM,
 * disk), moves into the VM's own directory and execs the VM's workload
 * under cpulimit. Never returns (either execs successfully or calls
 * exit(1) on failure).
 *
 * @param vm Pointer to the VM being spawned.
 * @param vm_dir Directory prepared by vm_prepare_directory(), used as the
 *               working directory of the workload.
 */
void spawn_vm_child(DataCenter *dc, VM *vm, const char *vm_dir);

/**
 * Forks one child per VM in the reservation, calling spawn_vm_child()
 * in each child and recording the pid/state in the parent.
 *
 * @param res Pointer to the Reservation whose VMs should be spawned.
 *
 * @return 0 on success.
 * @return 1 if any fork() call fails.
 */
int spawn_all_vms(DataCenter *dc, Reservation *res);

/**
 * Blocks until every running VM's process has exited, then destroys it using
 * vm_destroy(). The Reservation itself is embedded in DataCenter and is not
 * freed here.
 *
 * @param dc Pointer to the Data Center containing the reservation.
 * @param res Pointer to the Reservation whose VMs should be waited on.
 */
void wait_for_all_vms(DataCenter *dc, Reservation *res);

/**
 * Converts a VM state to a human-readable string. Used by datacenter_list().
 *
 * @param state The VMState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *vm_state_to_string(VMState state);

/**
 * Converts a reservation state to a human-readable string. Used by
 * datacenter_list().
 *
 * @param state The ReservationState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *res_state_to_string(ReservationState state);

#endif // DATACENTER_UTILS__H