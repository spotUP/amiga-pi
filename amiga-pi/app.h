/* app -- amiga-pi from its command line to the end (plan C7): the
 * arguments, the model and its key, the session with its system prompt,
 * then print mode or the prompt. The mains (main_amiga.c, main_posix.c) only
 * hand it the platform. Portable C89, host-tested. */
#ifndef PI_APP_H
#define PI_APP_H

#include "claude/ui.h"
#include "agent.h"

typedef struct pi_platform {
    cl_io *io;
    llm_pool *pool;
    llm_hooks h;                /* sleep, ms, resolve (the LAN rule), poll for print mode */
    llm_ts (*now)(void *u);
    /* an environment variable: its length, -1 none */
    long (*getenv)(const char *name, char *out, long cap);
    const char *cwd;
    /* the session once made, before the prompt (the tests add their tools here), 0 none */
    void (*ready)(pi_session *s);
} pi_platform;

/* the program: its exit code (0, 1 the answer failed, 2 bad arguments) */
int pi_app(int argc, char **argv, const pi_platform *pl);

#endif
