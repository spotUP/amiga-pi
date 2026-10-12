/* amiga-pi on the host (build/amiga-pi-host): the program is pi_host_run() (host_posix.c) */
#include "host_posix.h"

int main(int argc, char **argv)
{
    return pi_host_run(argc, argv, 0);
}
