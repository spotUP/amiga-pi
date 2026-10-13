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
 * One ordered list (plan L8), kept here and nowhere else: the hook commands (ext_hooks.h),
 * the Lua extensions (ext_lua.h) in load order, then the permission gate (ext_gate.h). An
 * extension names its rank (PI_EXT_HOOKS, PI_EXT_LUA, PI_EXT_GATE) and pi_ext_add puts it
 * in place, so the order does not depend on who registers first; an extension without a rank
 * (0) goes to the end in the order it was added. A hook that rewrites a call's arguments is
 * seen by the gate's decision, as Claude Code's PreToolUse hooks come before its permission
 * check.
 *   command     a slash command an extension registered (pi.registerCommand): the line
 *               "/name args" runs its handler before the input event and the prompt
 *               templates (agent-session.ts prompt() 1977-1983, _tryExecuteExtensionCommand
 *               2116-2142); get_commands and the slash menu list them, extension by extension
 *               in list order, each name resolved as runner.ts resolveRegisteredCommands.
 *   ui          pi's ExtensionUIContext dialogs (types.ts:149-160): select, confirm, input, notify. The mode that has a
 *               user binds them (pi_exts.ui: the TUI, RPC's extension_ui_request); print and json mode bind nothing,
 *               and the extension's ctx.hasUI is false (pi: noOpUIContext, runner.ts:324).
 * Portable C89. */
#ifndef PI_EXT_H
#define PI_EXT_H

#include "claude/json.h"
#include "events.h"

#define PI_EXTS 16
#define PI_EXT_CMDS 64                  /* extension commands registered at most */

/* the dialogs an extension can open (pi_exts.ui). A dialog waits for the user and the mode keeps its own loop running meanwhile
 * (the TUI's menu loop, RPC's line reader), so the extension's call returns with the answer. timeout_ms > 0 ends the dialog
 * by itself (pi's ExtensionUIDialogOptions.timeout); 0 waits. select and input give 1 and the text in value, or 0 for
 * cancelled and timed out (pi's undefined); confirm gives 1 only for yes (cancelled and timed out: false, pi's default). */
typedef struct pi_ext_ui {
    void *u;
    int (*select)(void *u, const char *title, const char *const *opt, int n, long timeout_ms, char *value, long cap);
    int (*confirm)(void *u, const char *title, const char *message, long timeout_ms);
    int (*input)(void *u, const char *title, const char *placeholder, long timeout_ms, char *value, long cap);
    /* type: "info", "warning", "error", or 0 */
    void (*notify)(void *u, const char *message, const char *type);
} pi_ext_ui;

/* the answer of one handler to an event of the generic kind (pi_exts_emit): answer is the object the handler returned (p 0
 * when it returned nothing or something else), after the event as the handler left it (only when pi_exts_emit was asked for
 * PI_EMIT_INPLACE and the handler changed it; p 0 otherwise). The function applies it, may rewrite the payload the next
 * handler is given, and answers 1 to end the dispatch (pi: a cancelling result, a handled input). */
typedef int (*pi_ext_apply)(void *u, jv answer, jv after, jw *payload);
#define PI_EMIT_INPLACE 1               /* handlers may change the event in place (pi's context, before_agent_start): read it back */

/* pi_ext.rank: where pi_ext_add puts the extension (0: no rank, the end) */
#define PI_EXT_HOOKS 1
#define PI_EXT_LUA 2
#define PI_EXT_GATE 3

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
    int rank;                   /* PI_EXT_*: the place in the list */
    /* the command registered as handle (pi_exts_add_command) run with args (the text after the first space, "" none):
     * 0 ran, -1 could not (the extension is gone); the handler's own failure is the extension's to report */
    int (*command)(void *u, int handle, const char *args);
    /* pi's SourceInfo of the extension for get_commands: its file, scope ("user", "project", "temporary") and baseDir;
     * 0 for a built-in */
    const char *path, *scope, *base_dir;
    /* every event of pi's ExtensionEvent union the dedicated members above do not carry (message_update, turn_end, context,
     * the session_* and model events ...): does the extension have a handler for pi's event type? */
    int (*has)(void *u, const char *type);
    /* the event delivered to every handler of the extension for type, one after the other, each given payload (pi's event
     * object as JSON) as it stands after the one before; fn (0: answers are ignored) sees each handler's answer. 1 when fn
     * ended the dispatch */
    int (*emit)(void *u, const char *type, jw *payload, int flags, pi_ext_apply fn, void *fu);
} pi_ext;

/* a command an extension registered; name and description belong to the owner */
typedef struct pi_ext_cmd {
    const pi_ext *owner;
    const char *name, *description;     /* description 0: none */
    int handle;
} pi_ext_cmd;

/* a command as the list shows it (get_commands, the slash menu): invocation is the name /invocation runs */
typedef struct pi_ext_rcmd {
    const pi_ext_cmd *c;
    char invocation[120];
} pi_ext_rcmd;

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
    const pi_ext_ui *ui;        /* the mode's dialogs, 0 none (print, json): ctx.hasUI is ui != 0 */
    int turn;                   /* turn_start's turnIndex: 0 at agent_start, +1 after each turn_end (kept by pi_exts_event) */
    int thrown;                 /* the tool itself failed: its result's details are {} when it has none */
    pi_ext_cmd cmd[PI_EXT_CMDS];
    int ncmd;
    int prompt_depth;           /* dialogs open at once: ui_prompt_start / _end are told for the outermost only (runner.ts withUIPrompt) */
    const char *cwd;            /* the working directory (resources_discover, user_bash carry it), "" until the mode sets it */
    /* pi's bindExtensions goes on after session_start with resources_discover: called once per binding, after the
     * extensions' session_start (RPC binds a replaced session twice, so twice) with the same reason */
    void (*bound)(void *u, const char *reason);
    void *bound_u;
} pi_exts;

void pi_exts_init(pi_exts *x);
/* put in its place by rank (after the ones of its rank before it); no rank: at the end. 0, -1 full */
int pi_ext_add(pi_exts *x, const pi_ext *e);
/* a slash command of an extension (pi.registerCommand): 0, -1 full. A name the owner registered before is replaced. */
int pi_exts_add_command(pi_exts *x, const pi_ext *owner, const char *name, const char *description, int handle);
/* the commands in list order with their invocation names (runner.ts resolveRegisteredCommands: a name more than one
 * extension registered is name:1, name:2, ... by occurrence, one taken already moves on to the next number); at most max,
 * the count */
int pi_exts_commands(const pi_exts *x, pi_ext_rcmd *out, int max);
/* agent-session.ts _tryExecuteExtensionCommand: the line "/name args" ran a registered command (by invocation name): 1,
 * 0 not one (the line goes on to the input event). */
int pi_exts_run_command(pi_exts *x, const char *line);
/* the emitter's listener (u: the pi_exts) */
void pi_exts_event(void *u, const pi_event *e);
/* 1 blocked (reason), 0 run it */
int pi_exts_tool_call(pi_exts *x, const char *id, const char *name, jw *args, char *reason, long cap);
void pi_exts_tool_result(pi_exts *x, const char *id, const char *name, const char *args, long an, jw *content,
                         int *is_error);
/* pi's emitInput: text rewritten by the extensions in order; 1 when one handled it (no run) */
int pi_exts_input(pi_exts *x, jw *text);
void pi_exts_session_start(pi_exts *x, const char *reason, const char *prev);
/* does any extension handle pi's event type (so the payload is worth building)? */
int pi_exts_has(const pi_exts *x, const char *type);
/* pi's runner emit for the events without a dedicated member: the payload to each extension in list order (see pi_ext.emit and
 * pi_ext_apply). 1 when an apply function ended the dispatch */
int pi_exts_emit(pi_exts *x, const char *type, jw *payload, int flags, pi_ext_apply fn, void *fu);

#endif
