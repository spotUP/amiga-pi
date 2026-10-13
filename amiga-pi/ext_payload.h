/* ext_payload -- what every extension kind (hook commands, Lua) is handed and what it may answer, in one place (plan L3).
 *
 * pi gives every handler the same event object (core/extensions/types.ts) and reads the same result shape back
 * (core/extensions/runner.ts); amiga-pi's extension kinds differ in how the event travels (a file for a hook command,
 * a table for a Lua handler) and in how the answer comes back (JSON on stdout, a returned table). So the payload is built
 * here as pi's JSON once, and the answer arrives here as a JSON object (a Lua table is encoded first): the decision rules
 * are applied by one function per event. Portable C89. */
#ifndef PI_EXT_PAYLOAD_H
#define PI_EXT_PAYLOAD_H

#include "ext.h"
#include "agent.h"

/* pi's event type for an emitter event (PE_*): "tool_execution_start", "agent_end", ...; 0 for the ones not dispatched */
const char *pi_ext_event_name(int pe_type);

/* ---- payloads (types.ts) ---- */
/* an emitter event (agent_start ... agent_settled): mode_json.c's record except turn_start (adds turnIndex and timestamp)
 * and agent_end (no willRetry: AgentEndEvent is {type, messages}); 0 when the event has no payload. b is appended to. */
int pi_ext_payload_event(pi_session *s, const pi_event *e, jw *b);
/* ToolCallEvent: toolName, toolCallId, input (agent-session.ts:668) */
void pi_ext_payload_call(jw *b, const char *name, const char *id, const jw *args);
/* ToolResultEvent (agent-session.ts:690): details and structuredContent are the running tool's (x->details, x->structured) */
void pi_ext_payload_result(jw *b, const pi_exts *x, const char *name, const char *id, const char *args, long an,
                           const jw *content, int is_error);
/* InputEvent (agent-session.ts:1927) */
void pi_ext_payload_input(jw *b, const jw *text, const char *source, const char *streaming);
/* SessionStartEvent */
void pi_ext_payload_start(jw *b, const char *reason, const char *prev);

/* UIPromptStartEvent / UIPromptEndEvent (types.ts:1014-1028): kind "select", "confirm", "input", "editor", "custom"; title 0: none */
void pi_ext_payload_ui_prompt(jw *b, int start, const char *kind, const char *title);
/* ModelSelectEvent (types.ts:1106): previous 0: undefined; source "set" or "cycle" */
void pi_ext_payload_model_select(jw *b, const llm_model *m, const llm_model *previous, const char *source);
/* ThinkingLevelSelectEvent (types.ts:1114) */
void pi_ext_payload_thinking(jw *b, const char *level, const char *previous);
/* SessionInfoChangedEvent (types.ts:750): name 0 is a cleared name (no key, as JSON.stringify drops undefined) */
void pi_ext_payload_session_info(jw *b, const char *name);
/* SessionShutdownEvent (types.ts:810): reason "quit", "reload", "new", "resume", "fork"; target 0: none */
void pi_ext_payload_shutdown(jw *b, const char *reason, const char *target);
/* SessionBeforeSwitchEvent (types.ts:757): reason "new" or "resume"; target 0: none */
void pi_ext_payload_before_switch(jw *b, const char *reason, const char *target);
/* SessionBeforeForkEvent (types.ts:764): position "before" or "at" */
void pi_ext_payload_before_fork(jw *b, const char *entry_id, const char *position);
/* ResourcesDiscoverEvent (types.ts:711) */
void pi_ext_payload_resources(jw *b, const char *cwd, const char *reason);
/* UserBashEvent (types.ts:1125) */
void pi_ext_payload_user_bash(jw *b, const char *command, int exclude, const char *cwd);

/* ---- decisions: the handler's answer as a JSON object ---- */
/* the session_before_* results (types.ts:1474-1500): {"cancel":true} -> 1 */
int pi_ext_decide_cancel(jv answer);
/* tool_call: {"block":true,"reason":"..."} -> 1 (reason set, "" for pi's default words); {"input":{...}} replaces args
 * (a hook command cannot mutate the event in place). runner.ts emitToolCall, agent-loop.ts. */
int pi_ext_decide_call(jv answer, jw *args, char *reason, long cap);
/* tool_result: content / details / structuredContent / isError each replace when given; content without
 * structuredContent drops the structured content (runner.ts emitToolResult) */
void pi_ext_decide_result(jv answer, pi_exts *x, jw *content, int *is_error);
/* input: {"action":"transform","text"} rewrites text, {"action":"handled"} -> 1 (runner.ts emitInput) */
int pi_ext_decide_input(jv answer, jw *text);

#endif
