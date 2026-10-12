/* agent -- amiga-pi's agent loop (plan C1), pi-agent-core's runAgentLoop
 * (pi f10993bc packages/agent/src/agent-loop.ts) in C:
 *   pi_prompt(s, text): the user's message, then rounds -- a request with
 *   the whole conversation (llm_transform: no failed answers, no orphaned
 *   tool calls), the answer streamed through the accumulator, its tool
 *   calls run one after another in message order (pi may run them in
 *   parallel), one toolResult each -- until an answer stops for anything
 *   but toolUse. stop and toolUse as pi; length: its tool calls are not
 *   run, each answered with pi's error text; error and aborted end the
 *   prompt. At most PI_ROUNDS rounds (C:Claude's cap, claude/repl.c).
 * Every step is an event through s->em (events.h); extensions (ext.h) may
 * block or rewrite a call and replace its result. The top-level entry of
 * every amiga-pi reachability test.
 * Portable C89 over llm/ (vtcon). */
#ifndef PI_AGENT_H
#define PI_AGENT_H

#include "llm/request.h"
#include "llm/accum.h"
#include "llm/handoff.h"
#include "llm/creds.h"
#include "llm/registry.h"
#include "events.h"
#include "ext.h"

#define PI_ROUNDS 64
#define PI_TOOLS 64
#define PI_QUEUE 16             /* steering and follow-up messages kept at most, each */

/* where pi checks the context against the window (agent-session.ts f10993bc): before a prompt's user
 * message is kept (prompt(), the last answer's usage), before each turn after the first (agent-loop's
 * prepareNextTurn, the projected estimate), after agent_end (_handlePostAgentRun, the run's last answer) */
#define PI_CP_PROMPT 0
#define PI_CP_TURN 1
#define PI_CP_END 2

struct pi_session;

typedef struct pi_tool {
    const char *name;
    const char *description;
    const char *parameters;     /* JSON schema of the arguments */
    const char *snippet;        /* the system prompt's one line, 0 none */
    void *u;
    /* the call: args a JSON object; the result's blocks appended to content
     * as a JSON array; *is_error. 0, or 1 when the user stopped it */
    int (*run)(void *u, struct pi_session *s, const char *args, long an, jw *content, int *is_error);
    long calls;                 /* the tests' sentinel */
    const char *const *guide;   /* its lines for the system prompt's rules (pi's promptGuidelines), 0 none */
} pi_tool;

typedef struct pi_session {
    llm_model model;
    const char *key;            /* 0 none: used when cfg is 0 */
    llm_cfg *cfg;               /* credentials and config values (models.json, auth.json, --api-key); 0: key */
    int thinking;               /* the thinking level, LT_*, as the model supports it */
    long ctx_tokens;            /* tokens the last answer reported (its input, output and cache): the context in use */
    long cost_micro;            /* what the answers of this run cost, micro-dollars, from the model's price */
    long tok_in, tok_out, tok_cr, tok_cw;   /* the answers' tokens summed (the footer's usage line) */
    char keybuf[512];           /* the credential of the request being made */
    const char *user_agent;
    llm_pool *pool;
    llm_hooks h;                /* poll (Ctrl+C), sleep, ms, notice, resolve, log */
    llm_ts (*now)(void *u);     /* the messages' timestamps (h.u) */
    const char *system;         /* the system prompt (prompt.c), 0 none */
    const char *session_id;     /* the session's id (pi's sessionId: openai-responses prompt_cache_key and headers), 0 none */
    pi_tool *tools[PI_TOOLS];
    int ntools;
    pi_emitter em;
    pi_exts ext;
    /* the conversation: pi-ai messages, each its own allocation */
    char **msg;
    long *msg_n;
    int nmsg, cap;
    /* the transcript's system messages (the session's), each before message sys_at[i]: a model that takes them
     * mid-conversation (pi-ai supportsMidConvoSystemMessages) gets them in place; any other model the replayed
     * s->system */
    char **sys;
    long *sys_n;
    int *sys_at;
    int nsys, syscap;
    int oom;
    long rounds;                /* rounds run since the start (the tests' sentinel) */
    int stop;                   /* the last answer's stop reason (LS_*) */
    const char *call_id;        /* the tool call running now (a tool names its kept output after it) */
    const char *shell_note;     /* the system prompt's sentence on the bash tool's shell, 0 none */
    const struct pi_res *res;   /* context files, SYSTEM.md, APPEND_SYSTEM.md for the prompt (resources.h), 0 none */
    const char *mcp_section;    /* pi's mcp_servers section (mcp.c), 0 none */
    /* the gate's question (ext_gate.h), set by the prompt's UI: ASK_* of
     * claude/tools.h; 0 nobody can answer (print mode) */
    int (*ask)(void *u, int tool, const char *name, const char *what, int outside);
    void *ask_u;
    /* the session store's automatic compaction (store.h), at pi's points (PI_CP_*); it may rewrite the
     * conversation, a failure is told by its own events and the prompt goes on */
    void (*compact)(void *u, struct pi_session *s, int when);
    void *compact_u;
    /* before a request to a model that takes tool changes in the transcript: the store declares the executable
     * tools' difference to the transcript as a system message (pi's agent-loop declareToolChanges) */
    void (*declare)(void *u, struct pi_session *s);
    void *declare_u;
    /* slash commands (store.c): a line the prompt got that starts with '/'. 1 when handled -- the lines to show
     * go to reply, and editor receives text the user takes back to edit (a message left by /tree); 0 not a command
     * of the store's (the line goes to the model) */
    int (*command)(void *u, struct pi_session *s, const char *line, jw *reply, jw *editor);
    void *command_u;
    /* steering and follow-up messages (pi-agent-core's steer / followUp, agent.ts PendingMessageQueue): typed
     * while a prompt runs; steering is taken after the tool calls of the turn that is running (before the next
     * request), follow-ups when the run would stop. Each queue gives one message a turn or all of them
     * (settings steeringMode / followUpMode "one-at-a-time", "all") */
    char *steer[PI_QUEUE], *follow[PI_QUEUE];
    int nsteer, nfollow;
    int steer_all, follow_all;
    char err[256];              /* its errorMessage, or why the prompt stopped */
} pi_session;

/* model copied; pool and hooks the caller's; extensions are subscribed to the events */
void pi_session_init(pi_session *s, const llm_model *m, const char *key, llm_pool *pool, const llm_hooks *h);
void pi_session_free(pi_session *s);
/* 0, -1 full */
int pi_tool_add(pi_session *s, pi_tool *t);
/* a message (pi-ai JSON) added to the conversation, copied: 0, -1 out of memory */
int pi_session_push(pi_session *s, const char *json, long n);
/* a system message of the transcript added at the end of the conversation (copied): 0, -1 out of memory */
int pi_session_push_system(pi_session *s, const char *json, long n);
/* the conversation emptied (a session loaded, a branch left) */
void pi_session_clear(pi_session *s);
/* the tools as pi-ai's request carries them: a JSON array of {name, description, parameters} */
void pi_tools_json(const pi_session *s, jw *w);

/* the credential and the extra headers of a request to s->model, filled into q (hdr holds the header text:
 * the caller frees it after the request); 0, or -1 with a message in s->err when a header value cannot be resolved */
int pi_req_auth(pi_session *s, llm_req *q, jw *hdr);

/* the prompt to the end: the last answer's stop reason (LS_STOP, LS_LENGTH,
 * LS_ERROR, LS_ABORTED; LS_TOOLUSE when the round cap stopped it) */
int pi_prompt(pi_session *s, const char *text);
/* queue a message while a prompt runs (copied): 0, -1 when the queue is full or memory is out. The run takes it
 * at pi's points (see pi_session.steer) */
int pi_steer(pi_session *s, const char *text);
int pi_follow_up(pi_session *s, const char *text);
/* every queued message (steering first) taken out of the queues: the texts malloc'ed into out (at most max),
 * their count; the caller frees them */
int pi_queue_take(pi_session *s, char **out, int max);
/* the text blocks of the last assistant message, joined (print mode): its length, out terminated */
long pi_last_text(const pi_session *s, jw *out);

/* calls of the loop's round since the start (agent_round in the plan) */
extern long pi_agent_rounds;

#endif
