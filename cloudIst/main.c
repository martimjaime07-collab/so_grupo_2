#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "parser.h"
#include "datacenter.h"
#include "../common/constants.h"
#include "../common/filesystem.h"

int main(int argc, char **argv){
    DataCenter dc;
    if(datacenter_init(&dc)) {
       return 1;
    }

    int result = 1;
    if (argc != 8 ) {
        fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <num_cores> <input_dir> <num_workers>\n", argv[0]);
        goto cleanup;
     }

    size_t servers;
    size_t ram;
    size_t disk;
    double cpu;
    size_t num_cores;
    size_t num_workers;
    char * input_path;

    if (parse_size_t_arg(argv[1], &servers) != 0 ||
            parse_size_t_arg(argv[2], &ram) != 0 ||
            parse_size_t_arg(argv[3], &disk) != 0 ||
            parse_double_arg(argv[4], &cpu) != 0 ||
            parse_size_t_arg(argv[5], &num_cores) != 0 ||
            parse_size_t_arg(argv[7], &num_workers) != 0 ||
            num_cores == 0 || num_workers == 0) {
        fprintf(stderr, "Invalid command line arguments.\n");
        goto cleanup;
    }

    input_path = argv[6];

    if (!path_exists(input_path)) {
        fprintf(stderr, "Invalid input path.\n");
        goto cleanup;
    }

    Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
    };

    if(datacenter_run(&dc, servers, &resources, input_path, num_workers, num_cores) != 0){
        fprintf(stderr, "Failed to configure Data Center.\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    // Stop the waiter and release Data Center resources on every exit path.
    datacenter_destroy(&dc);
    return result;
}