/* app -- amiga-pi from its command line to the end (plan C7): the
 * arguments, the model and its key, the session with its system prompt,
 * then print mode or the prompt. The mains (main_amiga.c, main_posix.c) only
 * hand it the platform. Portable C89, host-tested. */
#ifndef PI_APP_H
#define PI_APP_H

#include "claude/ui.h"
#include "agent.h"
#include "claude/sys.h"
#include "args.h"
#include "tools_pi.h"
#include "ext_gate.h"
#include "store.h"
#include "models.h"
#include "mcp.h"
#include "resources.h"

typedef struct pi_platform {
    cl_io *io;
    llm_pool *pool;
    llm_hooks h;                /* sleep, ms, resolve (the LAN rule), poll for print mode */
    llm_ts (*now)(void *u);
    /* an environment variable: its length, -1 none */
    long (*getenv)(const char *name, char *out, long cap);
    const char *cwd;
    /* the machine for the tools (claude/sys.h: sys_amiga, sys_posix), 0 no tools */
    cl_sys *sys;
    const char *tmp;            /* where cut tool output is kept: "T:" */
    const char *agent_dir;      /* settings.json's directory: "ENVARC:amiga-pi" (pi: ~/.pi/agent), 0 none */
    const char *data_dir;       /* what grows (claude/datadir.c: UP-Term:var/amiga-pi, else ENVARC:amiga-pi;
                                 * the agent dir on the host), 0 none */
    int shell;                  /* PI_SH_*: what the bash tool runs */
    int utc;                    /* the clock is UTC (host: timestamps end in Z); else the Amiga's local time */
    /* MCP over HTTP's connections (mcp_http.c), 0: pool's */
    llm_pool *mcp_pool;
    /* the session once made, before the prompt (the tests add their tools here), 0 none */
    void (*ready)(pi_session *s);
    /* json and rpc mode keep stdout for their records: the program's own lines (warnings, MCP news) go here, one line
     * without its line feed; 0 drops them */
    void (*diag)(const char *line);
    /* secure random bytes for /login (PKCE verifier, state, device id): 0, -1 when the system has none; 0 pointer: no login */
    int (*random)(void *u, unsigned char *buf, long n);
    void *random_u;
    /* the tests' OAuth server (login.h pi_auth.url_base); 0 in the programs */
    const char *oauth_url_base;
} pi_platform;

/* the runtime of one run: what pi_app builds from the command line. One per process (the pieces it points to are
 * static): the modes and the SDK (piapi.h) work on it. */
typedef struct pi_rt {
    pi_platform pl;             /* the platform as given, except pl.io: in json and rpc mode the diagnostics' */
    cl_io *io;                  /* the console the mode talks on */
    cl_io diag;                 /* json and rpc mode: pl.io, writes to pl.diag */
    int mode;                   /* PI_MODE_* */
    int trusted;
    pi_args *a;
    pi_session *s;
    pi_models *models;
    pi_tools *tools;
    pi_gate *gate;
    pi_store *store;
    mcp *mc;
    pi_res *res;
    struct pi_tmpls *tmpls;     /* prompt templates (templates.h) */
} pi_rt;

/* the runtime from the command line: 0, -1 when there is nothing to run (--list-models: done), else an exit code
 * (the message is on the console) */
int pi_rt_open(pi_rt *rt, const pi_platform *pl, int argc, char **argv);
/* the mode the arguments chose, to the end: the exit code */
int pi_rt_run(pi_rt *rt);
void pi_rt_close(pi_rt *rt);

/* the program: its exit code (0, 1 the answer failed, 2 bad arguments) */
int pi_app(int argc, char **argv, const pi_platform *pl);

#endif
