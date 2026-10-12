/* host_posix -- amiga-pi's platform on the host: stdin/stdout/stderr, POSIX TCP and /bin/sh (see host_posix.c) */
#ifndef PI_HOST_POSIX_H
#define PI_HOST_POSIX_H

#include "app.h"

/* the program: pi_app() over the host's platform. over (0 for the real thing): another transport, taken from it are the
 * pool, h.resolve and h.u; the console (stdin, stdout, stderr) is always the host's */
int pi_host_run(int argc, char **argv, const pi_platform *over);

#endif
