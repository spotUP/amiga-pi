/* ext_hooks -- hook commands on pi's extension events (plan L1, L2).
 *
 * pi 1.1.0 has no hook commands (its extensions are TypeScript modules), so this is amiga-pi's way to the same
 * events: the "hooks" object of ENVARC:amiga-pi/settings.json names commands per pi event, in C:Claude's hook
 * format (claude/config.c) with pi's event names as the keys:
 *   {"hooks": {"tool_call": [{"matcher": "bash", "hooks": [{"type": "command", "command": "myhook", "timeout": 5}]}]}}
 * Each command runs as `<command> < <file>` through claude/hooks.c's runner (hooks_run_raw), the file holding the
 * event as pi's types.ts gives it to a handler (core/extensions/types.ts), one JSON line. The matcher is C:Claude's,
 * matched on the tool's name for tool_call, tool_result and tool_execution_*, and empty for the other events.
 * The user's settings.json only; a project's hooks come with the trust-gated extension discovery (L3).
 *
 * What a command answers is pi's handler return value, as a JSON object on its output (nothing, other text or an
 * object that says nothing is "no decision"):
 *   tool_call    {"block": true, "reason": "..."} blocks (core/extensions/runner.ts emitToolCall: the first blocker
 *                wins; agent-loop.ts: the reason, else "Tool execution was blocked", is the error result's text);
 *                {"input": {...}} replaces the call's arguments for the hooks after it and the tool (pi mutates
 *                event.input in place; a command cannot, so it returns the whole object);
 *   tool_result  {"content": [...], "isError": bool, "details": ..., "structuredContent": ...} replace the result's
 *                parts (runner.ts emitToolResult: each field when given; content replaced without structuredContent
 *                drops the structured content); later hooks see the changes;
 *   input        {"action": "transform", "text": "..."} rewrites the prompt, {"action": "handled"} consumes it
 *                (runner.ts emitInput; "continue" or nothing keeps it);
 *   the others   (session_start, agent_start, turn_start, message_start, message_end, tool_execution_start / update /
 *                end, agent_end, agent_settled) are told, no answer is read.
 * A command that does not run, runs out of time or exits non-zero is pi's handler that throws: a tool_call hook
 * then blocks the call (agent-session.ts _beforeToolCall: the error's message is the result's text), any other
 * event goes on without the hook and the failure is told (llm_hooks notice, pi's extension_error).
 *
 * Not hooked yet (COMPATIBILITY.md): message_update (the cumulative message and `partial` of pi-ai's streaming parser),
 * turn_end (BoundaryState: entries, context preview), the session events beyond session_start, the model events.
 * Portable C89, host-tested (tests/test_pi_hooks.c). */
#ifndef PI_EXT_HOOKS_H
#define PI_EXT_HOOKS_H

#include "claude/hooks.h"
#include "agent.h"

typedef struct pi_hooks {
    pi_ext ext;
    pi_session *s;
    cl_settings cfg;
    cl_hooks h;
    int turn;                   /* turn_start's turnIndex: 0 at agent_start, +1 at each turn_end */
    long errors;                /* hooks that failed (told, not blocking) */
} pi_hooks;

/* the hook runner on cl sys, tmp (where the event file goes), cwd; not yet in the session's list (pi_hooks_add) */
void pi_hooks_init(pi_hooks *k, pi_session *s, cl_sys *sys, const char *tmp, const char *cwd);
void pi_hooks_free(pi_hooks *k);
/* a settings.json text: its "hooks" taken (and "disableAllHooks"); 0, -1 not a JSON object */
int pi_hooks_settings(pi_hooks *k, const char *json, long n, const char *name);
/* joins the session's extension list (after the ones already there) and its event emitter's listener is the list's */
int pi_hooks_add(pi_hooks *k);
/* are there hooks for any pi event? */
int pi_hooks_any(const pi_hooks *k);

extern long pi_hook_runs;       /* hook commands run (the reachability tests' sentinel) */

#endif
