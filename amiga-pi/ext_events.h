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

#endif
