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
 *               tool_result handlers returning {content, isError});
 *   input       the prompt text before templates expand it: rewrite it (pi:
 *               {action: "transform", text}) or consume it ({action:
 *               "handled"}: no run); the first "handled" wins;
 *   session_start the session began (startup) or was replaced (new, resume).
 * One ordered list: the built-in gate, the hook commands (ext_hooks.h) and later
 * extensions are dispatched in the order they were added (plan L8: the gate joins
 * the same list). Portable C89. */
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
    /* text: the prompt so far (JSON-free bytes), an extension may rewrite it (reset and write); source is pi's
     * InputSource ("interactive", "rpc", "extension"); 1 handled (the first one ends the dispatch) */
    int (*input)(void *u, const char *source, const char *streaming, jw *text);
    /* pi's SessionStartEvent: reason "startup", "reload", "new", "resume", "fork"; prev the previous session file, 0 none */
    void (*session_start)(void *u, const char *reason, const char *prev);
} pi_ext;

typedef struct pi_exts {
    const pi_ext *e[PI_EXTS];
    int n;
    long calls, blocks;         /* tool_call dispatches, blocks (the tests count them) */
    int replace_binds;          /* how many times the mode binds a replaced session (session_start per new / resume): 0 or 1 once; RPC 2 */
    const char *streaming;      /* the input event's streamingBehavior ("steer", "followUp") while a queued message is dispatched, else 0 */
    const char *source;         /* the input event's source for the prompt now running (0: "interactive") */
    /* the running tool's structured content and details, for a tool_result extension that replaces the content
     * (pi: structuredContent is dropped unless the handler returns it too); set by the loop, 0 outside it */
    jw *structured, *details;
    int thrown;                 /* the tool itself failed: its result's details are {} when it has none */
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
/* pi's emitInput: text rewritten by the extensions in order; 1 when one handled it (no run) */
int pi_exts_input(pi_exts *x, jw *text);
void pi_exts_session_start(pi_exts *x, const char *reason, const char *prev);

#endif
