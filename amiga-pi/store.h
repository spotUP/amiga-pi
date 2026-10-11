/* store -- the session of a running amiga-pi (plan E3b, E7; pi f10993bc
 * agent-session.ts persistence on message_end, sdk.ts session restore): glue
 * between the agent loop (agent.h) and the session file (session3.h).
 *
 *   - Every finished user, assistant and toolResult message is appended to
 *     the session tree as it ends (a message_end subscriber), whatever stopped
 *     the answer.
 *   - The system prompt lives in the transcript as pi's system message: at
 *     the start of a prompt the live sections and tools are compared with
 *     what the transcript replays; the first time all of them are written
 *     ({"role":"system","content":"","sections":{..},"toolsAdded":[..]}),
 *     later only a patch (changed sections, null for a removed one,
 *     toolsAdded / toolsRemoved). The request's system prompt is then the
 *     replay (ctx_system_text), so what is sent equals what the file says.
 *   - Opening: a new session (model_change and thinking_level_change first),
 *     --continue (the newest in the directory), --session <path or id
 *     prefix>, --no-session (the tree in RAM only). The branch's messages
 *     are loaded into the agent (convertToLlm), the model restored from
 *     the branch.
 * Portable C89, host-tested (tests/test_pi_store.c). */
#ifndef PI_STORE_H
#define PI_STORE_H

#include "agent.h"
#include "session3.h"
#include "context.h"
#include "prompt.h"

struct cp_reply;

enum { PS_MODE_NEW, PS_MODE_CONTINUE, PS_MODE_SESSION, PS_MODE_NONE };

typedef struct pi_store {
    pi_s3 s3;
    pi_session *s;
    struct pi_models *models;   /* /model and /thinking (models.h), 0: the model cannot be switched */
    cl_sys *sys;
    char data_dir[200];
    char cwd[256];
    int mode;
    int resumed;                /* an existing file was opened */
    jw system;                  /* the replayed system prompt: what the next request carries */
    int loading;                /* messages being put into the agent: nothing is appended */
    long persisted;             /* messages appended on message_end (the tests' sentinel) */
    long syncs;                 /* system messages appended */
    long compactions;           /* compaction entries appended */
    long keep, reserve;         /* compaction settings (settings.json "compaction"; pi's defaults) */
    int cp_enabled;
    char err[200];
} pi_store;

/* now: the clock of the entries; utc: it is UTC (timestamps end in Z) */
void pi_store_init(pi_store *st, cl_sys *sys, llm_ts (*now)(void *u), void *u, int utc);
void pi_store_free(pi_store *st);
/* the session per mode (arg: --session's path or id prefix); data_dir/sessions/<cwd's directory>.
 * 0, -1 with err */
int pi_store_open(pi_store *st, int mode, const char *data_dir, const char *cwd, const char *arg);
/* the model the opened session names (its branch's last model_change or answer): 1 with provider and
 * id, 0 none */
int pi_store_model(pi_store *st, char *provider, long pcap, char *id, long icap);
/* the thinking level the opened session's branch ends with: 1 with the level's name in out, 0 when it has
 * no thinking_level_change */
int pi_store_level(pi_store *st, char *out, long cap);
/* a model_change / thinking_level_change entry appended (/model, /thinking): 0, -1 */
int pi_store_model_change(pi_store *st, const char *provider, const char *id);
int pi_store_thinking(pi_store *st, const char *level);
/* settings.json's "compaction" object ({enabled, reserveTokens, keepRecentTokens}) taken; text a JSON object */
void pi_store_settings(pi_store *st, const char *text, long n);
/* /compact: the conversation summarized now (instructions 0 none). 0, -1 with the reason in err */
int pi_store_compact(pi_store *st, const char *instructions);

/* the branch's messages and the replayed system prompt put into the agent again (after a compaction, a tree
 * move, a fork): 0, -1 */
int pi_store_reload(pi_store *st);
/* one summarization request to the session's model (compact.h's cp_complete) */
int pi_store_complete(void *u, const char *prompt, long max_tokens, struct cp_reply *r);
/* the slash commands of the session (cmds.c): /tree /fork /clone /label /name /session; 1 when handled */
int pi_cmd(pi_store *st, const char *line, jw *reply, jw *editor);

/* tied to the agent: subscribed to its events, the branch's messages loaded, the system prompt the replay.
 * A new session records the model and the thinking level. 0, -1 */
int pi_store_attach(pi_store *st, pi_session *s);

#endif
