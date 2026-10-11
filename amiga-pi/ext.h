/* ext -- the extension dispatcher (plan C3): events in, decisions out.
 * Built-in C extensions register here (C:Claude's permission gate as one,
 * phase D); hook commands and Lua plug in through the same table later
 * (phase L). What an extension can do is pi's extension API's core:
 *   event       every agent event (pi.on(...)), in registration order;
 *   tool_call   before a tool runs: change its arguments in place (pi:
 *               mutate event.input), or block it with a reason (pi:
 *               {block: true, reason}); the first blocker wins and the
 *               model gets an isError toolResult with the reason;
 *   tool_result after it ran: replace its content and isError (pi:
 *               tool_result handlers returning {content, isError}).
 * Portable C89. */
#ifndef PI_EXT_H
#define PI_EXT_H

#include "claude/json.h"
#include "events.h"

#define PI_EXTS 16

typedef struct pi_ext {
    const char *name;
    void *u;
    void (*event)(void *u, const pi_event *e);
    /* 1 blocks the call (reason set, "" for pi's default words); args holds
     * the arguments as a JSON object, an extension may rewrite it */
    int (*tool_call)(void *u, const char *id, const char *name, jw *args, char *reason, long cap);
    /* content: the result's blocks (JSON array) to keep or rewrite */
    void (*tool_result)(void *u, const char *id, const char *name, const char *args, long an, jw *content,
                        int *is_error);
} pi_ext;

typedef struct pi_exts {
    const pi_ext *e[PI_EXTS];
    int n;
    long calls, blocks;         /* tool_call dispatches, blocks (the tests count them) */
} pi_exts;

void pi_exts_init(pi_exts *x);
/* registered after the ones before it: 0, -1 full */
int pi_ext_add(pi_exts *x, const pi_ext *e);
/* the emitter's listener (u: the pi_exts) */
void pi_exts_event(void *u, const pi_event *e);
/* 1 blocked (reason), 0 run it */
int pi_exts_tool_call(pi_exts *x, const char *id, const char *name, jw *args, char *reason, long cap);
void pi_exts_tool_result(pi_exts *x, const char *id, const char *name, const char *args, long an, jw *content,
                         int *is_error);

#endif
