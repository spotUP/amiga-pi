/* pstatus -- pi 1.1.0's Program Status Protocol (OSC 7501): the program tells the terminal whether it is idle, working,
 * blocked on the user, done or failed. After packages/tui/src/program-status.ts (the sequence) and
 * packages/coding-agent/src/modes/interactive/program-status-reporter.ts (which event means which state).
 * Reports are written only when enabled: pi enables them when the terminal answers its OSC 7501 query or
 * PI_PROGRAM_STATUS=1; amiga-pi writes no query (its start sequence is C:Claude's, claude/tui.c), so PI_PROGRAM_STATUS=1 is
 * the only way here (COMPATIBILITY.md: open question whether UP-Term should answer the query). Portable C89, host-tested. */
#ifndef PI_PSTATUS_H
#define PI_PSTATUS_H

#include "claude/json.h"
#include "events.h"

enum { PST_IDLE, PST_WORKING, PST_BLOCKED, PST_DONE, PST_ERROR, PST_CLEAR };
enum { PSK_NONE, PSK_PERMISSION, PSK_QUESTION, PSK_AUTH };

/* one report as pi's formatProgramStatus writes it: ESC ] 7501 ; state=..:app=..:kind=..:msg=<base64> ESC \ , appended to
 * out. app is dropped when it does not match [A-Za-z0-9_.+-]{1,32}; kind only for blocked; msg (control characters become
 * spaces, trimmed, at most 2048 bytes) only when it is not empty */
void pi_pstatus_format(jw *out, int state, const char *app, int kind, const char *msg);

typedef void (*pi_pstatus_write)(void *u, const char *s, long n);

typedef struct pi_pstatus {
    int enabled;                /* reports are written */
    pi_pstatus_write write;
    void *u;
    const char *app;            /* the program's name in the report */
    const char *(*name)(void *u);       /* the session's name, 0 or "" none */
    void *name_u;
    int run_active, compacting;
    int result_state;           /* the run's outcome, reported when it settles: PST_DONE or PST_ERROR, PST_IDLE */
    char result_msg[160];
    int rest_state;             /* the status while no run is active */
    char rest_msg[160];
    int blocked;                /* a dialog waits for the user */
    int blocked_kind;
    char blocked_msg[400];
    char last[900];             /* the last report written, no repeats */
    int reported;               /* a status is standing on the terminal */
} pi_pstatus;

void pi_pstatus_init(pi_pstatus *p, int enabled, pi_pstatus_write write, void *u, const char *app,
                     const char *(*name)(void *u), void *name_u);
/* the reporter's handleEvent: agent_start, message_end, compaction_start/end, agent_settled (aborted), session_info */
void pi_pstatus_event(pi_pstatus *p, const pi_event *e);
/* setBlocked: a dialog is open (kind, message) or closed (kind PSK_NONE) */
void pi_pstatus_blocked(pi_pstatus *p, int kind, const char *msg);
/* the first report when the screen starts (idle), and the `clear` when it stops (terminal.ts stop) */
void pi_pstatus_start(pi_pstatus *p);
void pi_pstatus_stop(pi_pstatus *p);

#endif
