/* events -- amiga-pi's agent events and their one emitter (plan C2), after
 * pi-agent-core's AgentEvent (pi f10993bc packages/agent/src/agent-loop.ts):
 * agent_start, turn_start, message_start / message_update / message_end,
 * tool_execution_start / update / end, turn_end, agent_end, in the order
 * pi emits them; compaction_start / compaction_end (agent-session.ts) with the
 * reason in stop (0 manual, 1 threshold) and, at the end, is_error with the
 * text in msg (the failure, or what was kept). Subscribers: the UI, later JSON mode and extensions. The
 * role cl_feed has for C:Claude (claude/repl.h), without touching it.
 * Portable C89. */
#ifndef PI_EVENTS_H
#define PI_EVENTS_H

#include "llm/event.h"           /* vtcon (VTCON_DIR on the include path) */

struct llm_acc;

enum {
    PE_AGENT_START, PE_TURN_START, PE_MESSAGE_START, PE_MESSAGE_UPDATE, PE_MESSAGE_END,
    PE_TOOL_START, PE_TOOL_UPDATE, PE_TOOL_END, PE_TURN_END, PE_AGENT_END,
    PE_COMPACTION_START, PE_COMPACTION_END,
    /* pi's session events (agent-session.ts) beyond pi-agent-core's: the run and its automatic work are over, a queue
     * changed, the thinking level or the session's name changed, the failed answer of a retry was dropped from the
     * context (an entry appended), an automatic retry begins and ends */
    PE_AGENT_SETTLED, PE_QUEUE_UPDATE, PE_THINKING_LEVEL, PE_SESSION_INFO, PE_ENTRY_APPENDED,
    PE_RETRY_START, PE_RETRY_END, PE_N
};

#define PI_SUBS 8

typedef struct pi_event {
    int type;                   /* PE_* */
    /* message_start / message_end: the message (pi-ai JSON); message_start
     * of an assistant answer that is still streaming: 0 (its role is set) */
    const char *msg;
    long msg_n;
    int role;                   /* LR_* (llm.h) of the message */
    /* message_update: the assistant stream's event (pi: assistantMessageEvent);
     * the partial message is the accumulator's, not rebuilt for every delta */
    const llm_event *ae;
    /* tool_execution_*: the call */
    const char *tool_id, *tool_name;
    const char *args;           /* its arguments, JSON object text */
    long args_n;
    /* tool_execution_end: the result's content (JSON array of blocks), isError */
    const char *result;
    long result_n;
    int is_error;
    int stop;                   /* turn_end, agent_end: the last answer's stop reason (LS_*) */
    /* JSON the mode that prints events (mode_json.c) needs and the loop has at hand:
     *   message_update: acc is the answer's accumulator (the block's text, the call's arguments, the usage so far)
     *   turn_end: result is the turn's toolResult messages, a JSON array
     *   agent_end: result is the messages the run made (the transcript's system messages among them), a JSON array,
     *     and retry says a retry follows
     *   tool_execution_end: structured is pi's structuredContent of the result (a JSON value), 0 none
     *   tool_execution_update: result is the partial result's content, structured its details (a JSON value), 0 none
     *   compaction_end: result is the CompactionResult object, is_error with the failure in msg, retry willRetry,
     *     aborted
     *   thinking_level_changed: msg is the level; session_info_changed: msg is the name, 0 cleared
     *   entry_appended: msg is the entry's line
     *   auto_retry_start: n1 attempt, n2 maxAttempts, n3 delayMs, msg errorMessage;
     *     auto_retry_end: retry is success, n1 attempt, msg finalError (failure only) */
    const struct llm_acc *acc;
    const char *structured;
    long structured_n;
    int retry, aborted;
    /* tool_execution_end: durationMs (pi-agent-core 1.1.0), -1 when the tool did not run: no key;
     * agent_settled: aborted is pi's _agentRunAbortRequested */
    long duration_ms;
    long n1, n2, n3;
} pi_event;

typedef void (*pi_listener)(void *u, const pi_event *e);

typedef struct pi_emitter {
    pi_listener fn[PI_SUBS];
    void *u[PI_SUBS];
    int n;
    long count[PE_N];           /* events emitted, per type */
} pi_emitter;

void pi_emitter_init(pi_emitter *em);
/* 0, -1 full */
int pi_subscribe(pi_emitter *em, pi_listener fn, void *u);
/* the subscriber fn/u taken out again (1 it was there, 0 not) */
int pi_unsubscribe(pi_emitter *em, pi_listener fn, void *u);
/* every subscriber, in the order they subscribed */
void pi_emit(pi_emitter *em, const pi_event *e);
/* an event of type t with every field empty */
void pi_event_init(pi_event *e, int t);
/* pi's name: "agent_start", "tool_execution_end", ... */
const char *pi_event_name(int type);

#endif
