/* ext_events -- pi's extension events from the places they happen (plan L7).
 *
 * ext.h carries the events that have a member of their own (tool_call, tool_result, input, session_start) and the
 * agent events of events.h; every other member of pi's ExtensionEvent union (core/extensions/types.ts:1377) goes
 * through here: one function per event builds the payload (ext_payload.c), asks the extensions through pi_exts_emit
 * (hook commands and Lua handlers alike) and applies what they answered with the decision rules of ext_payload.c.
 * Nothing is built when no extension has a handler for the event (pi: hasHandlers). A call from a place that has no
 * session (pi_session 0) is a no-op. Portable C89, host-tested (tests/test_pi_hooks.c, test_pi_lua.c, test_pi_modes.c). */
#ifndef PI_EXT_EVENTS_H
#define PI_EXT_EVENTS_H

#include "agent.h"

/* the session's extension events are bound: resources_discover follows each session_start (pi_exts.bound) */
void pi_ev_attach(pi_session *s);

/* ui_prompt_start / ui_prompt_end (runner.ts withUIPrompt): a blocking dialog of an extension opens and closes */
void pi_ev_ui_prompt(pi_session *s, int start, const char *kind, const char *title);
/* model_select (agent-session.ts _emitModelSelect: not when the model is the same); source "set" or "cycle" */
void pi_ev_model_select(pi_session *s, const llm_model *model, const llm_model *previous, const char *source);
/* thinking_level_select (agent-session.ts setThinkingLevel, when the level changed) */
void pi_ev_thinking(pi_session *s, const char *level, const char *previous);
/* session_info_changed (agent-session.ts:3944); name 0 cleared */
void pi_ev_session_info(pi_session *s, const char *name);
/* session_shutdown (agent-session-runtime.ts:171, :405; main.ts at exit): reason "quit", "reload", "new", "resume", "fork" */
void pi_ev_shutdown(pi_session *s, const char *reason, const char *target);
/* session_before_switch / session_before_fork (agent-session-runtime.ts:143, :160): 1 when an extension cancelled */
int pi_ev_before_switch(pi_session *s, const char *reason, const char *target);
int pi_ev_before_fork(pi_session *s, const char *entry_id, const char *position);

/* turn_end (agent-session.ts _dispatchTurnEndBoundary): message is the turn's assistant message, results a JSON array of its tool
 * results. Handlers may answer {entries: [custom | custom_message drafts], continue}; the drafts are appended (entry_appended).
 * 1 when the run is to go on though the model asked for nothing more */
int pi_ev_turn_end(pi_session *s, const char *message, long message_n, const char *results, long results_n);
/* agent_before_settle (_runBeforeSettleBoundary): the same answers; 1 when the run is to continue */
int pi_ev_before_settle(pi_session *s);
/* context and context_with_system (runner.ts emitContext) over the transcript the next request carries: 1 when a handler
 * changed it, out then holds the new messages as a JSON array (system messages included, as the request needs them) */
int pi_ev_context(pi_session *s, jw *out);
/* before_agent_start (runner.ts emitBeforeAgentStart): handlers may answer {message}, a custom message that joins the user's
 * (kept in s->next_custom); systemPrompt overrides are not read */
void pi_ev_before_agent_start(pi_session *s, const char *prompt);

/* user_bash (runner.ts emitUserBash, rpc-mode.ts bash, interactive-mode.ts handleBashCommand): 1 when a handler answered
 * {result: {output, exitCode, cancelled, truncated}} -- text, code and cancelled then hold it and the command is not run. A
 * handler's {operations} (custom backends, TypeScript objects) has no meaning here and is ignored */
int pi_ev_user_bash(pi_session *s, const char *command, int exclude, jw *text, int *code, int *cancelled);

/* session_before_compact (agent-session.ts:2790, :3126): prep is the CompactionPreparation object, branch the JSON array of the
 * branch's entries, instr the custom instructions (0 none); reason 0 manual, 1 threshold, 2 overflow. 0 go on, 1 an extension
 * cancelled, 2 an extension made the compaction: comp holds its {summary, firstKeptEntryId, tokensBefore, details?, usage?} */
int pi_ev_before_compact(pi_session *s, const char *prep, const char *branch, const char *instr, int reason, int will_retry,
                         jw *comp);
/* session_compact (entry: the saved compaction entry's line) and session_compact_failed */
void pi_ev_compact(pi_session *s, const char *entry, long entry_n, int from_ext, int reason, int will_retry);
void pi_ev_compact_failed(pi_session *s, int reason, const char *error, int aborted, int will_retry, int from_ext);

#endif
