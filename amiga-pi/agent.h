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
    const char *constrained;    /* pi's constrainedSampling of the declaration, JSON text ({"type":"json_schema","strict":"prefer"}), 0 none */
} pi_tool;

typedef struct pi_session {
    llm_model model;
    const char *key;            /* 0 none: used when cfg is 0 */
    llm_cfg *cfg;               /* credentials and config values (models.json, auth.json, --api-key); 0: key */
    int thinking;               /* the thinking level, LT_*, as the model supports it */
    unsigned think_budget_set;  /* settings.json thinkingBudgets: bit LT_x set when that level has a custom budget (google, anthropic-messages, bedrock) */
    long think_budget[8];       /* its tokens, by LT_* */
    long ctx_tokens;            /* tokens the last answer reported (its input, output and cache): the context in use */
    long cost_micro;            /* what the answers of this run cost, micro-dollars, from the model's price */
    long tok_in, tok_out, tok_cr, tok_cw;   /* the answers' tokens summed (the footer's usage line) */
    char keybuf[4096];          /* the credential of the request being made (an OAuth access token is a JWT of a few KB) */
    /* OAuth (login.h): renews the stored credential of the model's provider before a request, or (force) after a 401;
     * 0 or -1 with the message in err. auth_u: the pi_auth */
    int (*auth_hook)(void *u, struct pi_session *s, int force, char *err, long cap);
    void *auth_u;
    char oauth_base[300];       /* the base URL a credential brings (github-copilot) for the request being made */
    const char *user_agent;
    llm_pool *pool;
    llm_hooks h;                /* poll (Ctrl+C), sleep, ms, notice, resolve, log */
    llm_ts (*now)(void *u);     /* the messages' timestamps (h.u) */
    const char *system;         /* the system prompt (prompt.c), 0 none */
    const char *session_id;     /* the session's id (pi's sessionId: openai-responses prompt_cache_key and headers), 0 none */
    pi_tool *tools[PI_TOOLS];
    int ntools;
    pi_tool *user_bash;         /* the bash backend of user bash (!cmd, RPC bash): set whether or not bash is in the active list (agent-session.ts executeBash) */
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
    const char *call_args;      /* its arguments as the model sent them (the tool_execution_update events carry these), 0 outside a call */
    long call_args_n;
    const char *shell_note;     /* the system prompt's sentence on the bash tool's shell, 0 none */
    const struct pi_res *res;   /* context files, SYSTEM.md, APPEND_SYSTEM.md for the prompt (resources.h), 0 none */
    const struct pi_tmpls *templates;   /* prompt templates: pi_prompt expands "/name args" (templates.h), 0 none */
    const char *mcp_section;    /* pi's mcp_servers section (mcp.c), 0 none */
    /* the gate's question (ext_gate.h), set by the prompt's UI: ASK_* of
     * claude/tools.h; 0 nobody can answer (print mode) */
    int (*ask)(void *u, int tool, const char *name, const char *what, int outside);
    void *ask_u;
    /* the session store's automatic compaction (store.h), at pi's points (PI_CP_*); it may rewrite the
     * conversation, a failure is told by its own events and the prompt goes on. At PI_CP_END it answers 1 when it
     * compacted to recover from a context overflow and the answer is to be asked again, once (pi's _checkCompaction
     * returning true: the post-run loop calls agent.continue()); 0 otherwise */
    int (*compact)(void *u, struct pi_session *s, int when);
    void *compact_u;
    /* pi's preflightResult("started"): called once by pi_prompt after the check before the prompt (its compaction
     * events come first, as in pi's prompt()), then cleared; RPC mode answers the prompt command there */
    void (*started)(void *u);
    void *started_u;
    const char *disposition;    /* what started reports: 0 "started", "handled" (an input extension consumed the prompt) */
    /* before a request to a model that takes tool changes in the transcript: the store declares the executable
     * tools' difference to the transcript as a system message (pi's agent-loop declareToolChanges) */
    void (*declare)(void *u, struct pi_session *s);
    void *declare_u;
    /* slash commands (store.c): a line the prompt got that starts with '/'. 1 when handled -- the lines to show
     * go to reply, and editor receives text the user takes back to edit (a message left by /tree); 0 not a command
     * of the store's (the line goes to the model) */
    int (*command)(void *u, struct pi_session *s, const char *line, jw *reply, jw *editor);
    void *command_u;
    /* /reload (interactive mode): the skills, prompt templates and context files read again (app.c). 0 done, -1 failed (err says why); 0: none */
    int (*reload)(void *u, struct pi_session *s, char *err, long cap);
    void *reload_u;
    /* Ctrl+P, Shift+Ctrl+P (what 0, dir 1 / -1) and Shift+Tab (what 1): the next model / thinking level as pi cycles them;
     * the line pi shows in msg. 1 changed, 0 not, -1 failed; 0: nobody cycles */
    int (*cycle)(void *u, struct pi_session *s, int what, int dir, char *msg, long cap);
    void *cycle_u;
    /* steering and follow-up messages (pi-agent-core's steer / followUp, agent.ts PendingMessageQueue): typed
     * while a prompt runs; steering is taken after the tool calls of the turn that is running (before the next
     * request), follow-ups when the run would stop. Each queue gives one message a turn or all of them
     * (settings steeringMode / followUpMode "one-at-a-time", "all") */
    char *steer[PI_QUEUE], *follow[PI_QUEUE];
    int nsteer, nfollow;
    int steer_all, follow_all;
    /* the messages polled from a queue and not yet in the conversation (they leave the queue the session shows -- queue_update --
     * when their user message is made); pend_follow: they came from the follow-up queue */
    char *pend[PI_QUEUE];
    int npend, pend_follow;
    /* automatic retry of a failed answer (agent-session.ts _prepareRetry; settings.json "retry"): on, how many times,
     * the first delay in ms (it doubles, retry_max_delay at most: settings retry.maxAgentDelayMs, 60 s). attempt counts the retries of the failure being retried; retry_stop
     * cuts the wait (abort_retry) */
    int retry_on, retry_max;
    long retry_base, retry_max_delay;
    int attempt, retry_stop;
    /* the last run's agent_end decided to retry (pi's willRetry): the only place that decides, since giving up
     * (the last retry failed too) resets attempt, and a second look would start the retries over, without end */
    int retry_next;
    int recover;                /* the run ended on a context overflow the store compacted: ask again (pi_prompt) */
    /* pi's _overflowRecoveryAttempted: set by the store when it omits a failed answer and compacts to ask again; cleared
     * as pi's _handleAgentEvent does, at every user message (the prompt, steering, a follow-up) and at every answer that
     * is neither an error nor a length stop -- so a retried answer that calls a tool gives the run its allowance back */
    int overflow_tried;
    /* the failed answer, message index i, kept out of the context for the retry (the store appends pi's context_edit
     * entry); 0, -1 */
    int (*omit)(void *u, struct pi_session *s, int i);
    void *omit_u;
    /* pi.appendEntry (agent-session.ts bindExtensions' appendEntry): a custom entry -- customType and data, a JSON value or 0
     * -- appended to the session and announced as entry_appended; 0, -1 */
    int (*append_entry)(void *u, struct pi_session *s, const char *custom_type, const char *data);
    void *append_u;
    int result_error;           /* the tool running now returned a result with isError set, which is no thrown error (an extension's
                                 * tool; its details stay as it gave them): reset by the loop before each call */
    int active;                 /* a prompt is running (pi's isStreaming) */
    int retry_waiting;          /* the run waits out a retry delay (abort_retry cuts only this) */
    int aborted;                /* pi's _agentRunAbortRequested (agent-session.ts abort(), 1.1.0): set by the abort of a run in
                                 * progress (the RPC abort, Ctrl+C), by nothing else (not abort_retry, not abort_bash); cleared
                                 * at the start of the next run; agent_settled reports it */
    int run_msg0, run_sys0;     /* the conversation's size when the run began: agent_end's messages are what came after */
    int sys_seen;               /* the transcript's system messages announced as message events so far */
    jw structured;              /* the tool call running now: pi's structuredContent of its result (JSON), empty none */
    jw details;                 /* the same call's result.details (a JSON object: an MCP tool's server, tool, fullOutputPath), empty none */
    char err[LLM_ERR_MAX];       /* its errorMessage, or why the prompt stopped */
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
/* llm_req.reauth for a stored login: renews it once after a 401 (see pi_req_auth) */
int pi_req_reauth(void *u, llm_req *q, char *err, long cap);

/* the prompt to the end: the last answer's stop reason (LS_STOP, LS_LENGTH,
 * LS_ERROR, LS_ABORTED; LS_TOOLUSE when the round cap stopped it). A retryable failure (agent-session.js
 * isRetryableAssistantError) is retried as pi does when s->retry_on; agent_settled is the last event. */
int pi_prompt(pi_session *s, const char *text);
/* checkAuth: the model has a credential, or needs none (a LAN endpoint on plain http); cfg 0 passes */
int pi_auth_ok(llm_cfg *cfg, const llm_model *mo);
/* the conversation from message m0 and system message sys0 on, as a JSON array of pi-ai messages (the transcript's system
 * messages in their places) */
void pi_messages_json(const pi_session *s, int m0, int sys0, jw *w);
/* the queues' text as the session shows them (queue_update's two arrays), JSON arrays of strings appended to w */
void pi_queue_json(const pi_session *s, int follow, jw *w);
/* the same count */
int pi_queue_len(const pi_session *s, int follow);
/* pi's isRetryableAssistantError on an errorMessage: 1 when it looks like a transient provider or transport failure */
int pi_retryable(const char *error_message);
/* pi-ai's isContextOverflow on an errorMessage (the error cases): 1 when the request outgrew the window; such a failure is not retried */
int pi_context_overflow(const char *error_message);
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
extern long pi_agent_prompts;       /* pi_prompt calls (the reachability tests' sentinel, with the rounds) */

#endif
