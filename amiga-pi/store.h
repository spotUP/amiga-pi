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
    char agent_dir[200];        /* where trust.json is (/trust), "" none */
    char cwd[256];
    int mode;
    int resumed;                /* an existing file was opened */
    jw system;                  /* the replayed system prompt: what the next request carries */
    jw cp_result;               /* the last compaction's pi CompactionResult (JSON object), empty when it failed */
    int loading;                /* messages being put into the agent: nothing is appended */
    long persisted;             /* messages appended on message_end (the tests' sentinel) */
    long syncs;                 /* system messages appended */
    long compactions;           /* compaction entries appended */
    long keep, reserve;         /* compaction settings (settings.json "compaction"; pi's defaults) */
    int cp_enabled;
    int recovered;              /* a context overflow was compacted and the answer asked again since the last prompt (pi's
                                 * _overflowRecoveryAttempted): the next one ends the run */
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
/* the slash commands of the session (cmds.c): /tree /fork /clone /label /name /session /trust; 1 when handled */
int pi_cmd(pi_store *st, const char *line, jw *reply, jw *editor);

/* ! and !! (pi executeBash): a bashExecution message recorded in the session; unless exclude (!!) the agent gets it as
 * the user message pi's convertToLlm makes of it ("Ran `cmd`" and the output), for the next request. 0, -1 */
int pi_store_bash(pi_store *st, const char *command, const char *output, int exit_code, int cancelled, int exclude);

/* /fork N and /clone: a new session from the branch ending at entry leaf (-1: an empty one with this as parentSession),
 * the session continued there. 0, or -1 with the reason in reply ("[ERROR] ...") */
int pi_store_branch(pi_store *st, int leaf, jw *reply);
/* the text of the user message entry i, as the input takes it back (the text blocks joined) */
void pi_user_text(pi_s3 *s, int i, jw *out);
/* pi's newSession: an empty session in the same directory (parentSession: parent, "" none), the agent's conversation
 * cleared and the session's model and thinking level recorded again. 0, -1 with err */
int pi_store_new_session(pi_store *st, const char *parent);
/* pi's switchSession: the session file opened and its conversation loaded; the model and thinking level it names are
 * restored. 0, -1 with err (the current session is kept) */
int pi_store_switch(pi_store *st, const char *path);
/* pi's setSessionName: a session_info entry and the session_info_changed event. 0, -1 */
int pi_store_name(pi_store *st, const char *name);
/* a bash command run through the session's bash tool and recorded as !cmd is (pi executeBash): its output in text, the
 * exit code, whether it was stopped. 0, -1 when there is no bash tool */
int pi_store_bash_run(pi_store *st, const char *command, int exclude, jw *text, int *code, int *stopped);

/* a command run through the session's bash tool: its output in text, the exit code (pi keeps it apart from the text), whether
 * it was stopped. 0, -1 when there is no bash tool. Nothing is recorded (pi_store_bash_run does that) */
int pi_bash_exec(pi_session *s, const char *command, jw *text, int *code, int *stopped);

/* the leaf moved to entry idx as /tree N does (summarize, instr, replace: pi's navigateTree options): the lines to show
 * in reply ("[ERROR] ..." when it failed), the text of a user message left in editor */
void pi_tree_navigate(pi_store *st, int idx, int summarize, const char *instr, int replace, jw *reply, jw *editor);

/* tied to the agent: subscribed to its events, the branch's messages loaded, the system prompt the replay.
 * A new session records the model and the thinking level. 0, -1 */
int pi_store_attach(pi_store *st, pi_session *s);

#endif
