/* models -- the models amiga-pi can use and the switch between them (plan phase F): the registry with its bundled
 * catalog and ENVARC:amiga-pi/models.json laid over it, the credentials (--api-key, auth.json, models.json,
 * environment), and pi's setModel / setThinkingLevel on a running session (agent-session.ts :2476-:2630):
 *   - /model switches the session's model and appends a model_change entry; the thinking level is clamped to the
 *     new model and a thinking_level_change entry is appended only when it changed;
 *   - /thinking sets the level, clamped to what the model supports, an entry only when it changed.
 * The conversation is kept; the next request is handed off to the new model (llm/handoff.c).
 * Portable C89, host-tested (tests/test_pi_model.c, test_pi_model_reach.c). */
#ifndef PI_MODELS_H
#define PI_MODELS_H

#include "agent.h"
#include "claude/sys.h"

struct pi_store;

typedef struct pi_models {
    llm_reg reg;
    llm_cfg cfg;
    cl_sys *sys;                /* runs a `!command` value; 0: commands do not resolve */
    long (*getenv)(const char *name, char *out, long cap);
    char *auth;                 /* auth.json text, owned */
    char warn[320];             /* what was wrong with models.json, "" nothing */
    struct pi_store *store;     /* the session file the switches are recorded in, 0 none */
    char *scope;                /* settings.json enabledModels, the patterns one to a line (owned), 0 none: Ctrl+P cycles those */
    int default_level;          /* settings.json defaultThinkingLevel (LT_*), -1 none: a switch applies it (pi's getDefaultThinkingLevel) */
} pi_models;

/* a config value's `!command` (llm_cfg.run): pi's 10 second limit, the trimmed standard output, -1 failed or empty;
 * models.c's registry and mcp.c's values run commands through this one function */
long pi_cfg_run(cl_sys *sys, const char *cmd, char *out, long cap);

/* the catalog, then <agent dir>/models.json and auth.json (either may be missing). 0, -1 out of memory;
 * a bad models.json is only a warning (m->warn): the catalog stays as it was */
int pi_models_init(pi_models *m, cl_sys *sys, const char *agent_dir, long (*getenv)(const char *, char *, long));
void pi_models_free(pi_models *m);
/* the session takes the registry's credentials; the thinking level is clamped to s->model */
void pi_models_attach(pi_models *m, pi_session *s, struct pi_store *st);

/* the thinking level the session starts with: `want` (LT_*, -1: settings.json's, else pi's default "medium"),
 * clamped to the model; "off" for a model without reasoning */
int pi_models_start_level(const llm_model *model, int want);

/* pi's setModel: 0, or -1 with err ("No API key for provider/id") */
int pi_model_switch(pi_models *m, pi_session *s, const llm_model *to, char *err, long cap);
/* the same with the level the model starts at given (a scoped model's ":level"; -1: the usual rule) */
int pi_model_switch_level(pi_models *m, pi_session *s, const llm_model *to, int explicit_level, char *err, long cap);
/* pi's setThinkingLevel: the level in effect afterwards, -1 when the session file cannot take the entry (the level is unchanged) */
int pi_thinking_set(pi_models *m, pi_session *s, int level);
/* "provider/id" of the model */
void pi_model_label(const llm_model *m, char *out, long cap);

/* settings.json's enabledModels (patterns joined by newlines, 0 none): what Ctrl+P cycles through */
int pi_models_set_scope(pi_models *m, const char *patterns);
/* pi's cycleModel (agent-session.js :1980): the next (dir 1) or previous (dir -1) model of the scope -- the models of
 * enabledModels that have a credential, in the order the patterns give them (resolveModelScopeFromModels: globs over
 * provider/id and id, ":level" for the level of that model) -- or, with no scope, of all models that have one. Switched
 * the way /model does (model_change, thinking_level_change when it changed). 1 switched, 0 nothing to switch to, -1 the
 * session file could not take it; msg is the line pi shows ("Switched to x (thinking: high)", "Only one model in scope") */
int pi_model_cycle(pi_models *m, pi_session *s, int dir, char *msg, long cap);
/* pi's cycleThinkingLevel: the next level the model supports, wrapping; msg "Thinking level: x" or "Current model
 * does not support thinking". 1 changed, 0 not, -1 not recorded */
int pi_thinking_cycle(pi_models *m, pi_session *s, char *msg, long cap);

/* the lines of /model: every model whose provider has a credential, numbered, the current one marked */
void pi_models_list(pi_models *m, const pi_session *s, jw *out);
/* the model the Nth line of pi_models_list names (1-based): 0 none */
const llm_model *pi_models_nth(pi_models *m, const pi_session *s, int n);

#endif
