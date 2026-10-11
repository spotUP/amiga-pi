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

enum {
    PE_AGENT_START, PE_TURN_START, PE_MESSAGE_START, PE_MESSAGE_UPDATE, PE_MESSAGE_END,
    PE_TOOL_START, PE_TOOL_UPDATE, PE_TOOL_END, PE_TURN_END, PE_AGENT_END,
    PE_COMPACTION_START, PE_COMPACTION_END, PE_N
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
/* every subscriber, in the order they subscribed */
void pi_emit(pi_emitter *em, const pi_event *e);
/* an event of type t with every field empty */
void pi_event_init(pi_event *e, int t);
/* pi's name: "agent_start", "tool_execution_end", ... */
const char *pi_event_name(int type);

#endif
