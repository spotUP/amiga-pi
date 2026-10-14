/* scoped -- the models Ctrl+P cycles through, edited as pi's /scoped-models does (scoped-models-selector.ts, interactive-mode.ts
 * showModelsSelector): the list of models that have a credential, the enabled ones in their order (null: all of them), and the
 * operations of the selector -- toggle, enable all, clear all, a provider's models, reorder -- over strings "provider/id".
 * pi_scope_apply puts the list on the session at once (setScopedModels: only for a list that has an available model and leaves
 * one out); pi_scope_save writes it to settings.json's enabledModels (a list that covers every available model removes the
 * member). The screen (ui_pi.c) and the line mode (cmds.c) both drive this; neither keeps a list of its own. Portable C89. */
#ifndef PI_SCOPED_H
#define PI_SCOPED_H

#include "models.h"

typedef struct pi_scope_item {
    const llm_model *m;         /* 0: an enabled entry that no available model answers to */
    char *id;                   /* the pattern of an unavailable entry (points into the scope, valid until the next change); 0 for a model: pi_scope_item_id */
    int enabled;
} pi_scope_item;

typedef struct pi_scope {
    pi_models *ms;
    const llm_model **all;      /* the available models, in the registry's order */
    int nall;
    char **en;                  /* enabledIds, malloc'ed strings */
    int nen, encap;
    int en_null;                /* null: every model is enabled */
    pi_scope_item *item;        /* the list as the selector shows it: the enabled ones first, then the rest (getSortedIds) */
    int nitem;
} pi_scope;

/* the available models and the enabled list the configured scope (settings.json enabledModels, --models) gives: 0, -1 out of memory */
int pi_scope_open(pi_scope *s, pi_models *ms);
void pi_scope_free(pi_scope *s);
/* "provider/id" of a model */
void pi_scope_id(const llm_model *m, char *out, long cap);
int pi_scope_is_enabled(const pi_scope *s, const char *id);
/* the entry's "provider/id" (or pattern, for an unavailable one) */
void pi_scope_item_id(const pi_scope_item *it, char *out, long cap);

/* the selector's operations; each rebuilds item[] */
void pi_scope_toggle(pi_scope *s, const char *id);
void pi_scope_enable(pi_scope *s, const char *const *ids, int n);       /* n < 0: every model */
void pi_scope_clear(pi_scope *s, const char *const *ids, int n);        /* n < 0: every enabled one */
void pi_scope_move(pi_scope *s, const char *id, int delta);             /* 1 when the entry moved */
/* the enabled models of one provider switched together: off when all of them are on, else on */
void pi_scope_provider(pi_scope *s, const char *provider);

/* "N/M enabled" ("all enabled", with " . N unavailable" appended as pi words it) */
void pi_scope_count_text(const pi_scope *s, char *out, long cap);
/* updateSessionModels: the session's scope follows the list. 0, -1 out of memory */
int pi_scope_apply(pi_scope *s);
/* onPersist: settings.json's enabledModels is the list, or absent when it is null or covers every available model. 0, -1 failed */
int pi_scope_save(pi_scope *s, cl_sys *sys, const char *dir);

/* agent-session.ts _addPersistedDefaultToNonEmptyScope: a model chosen as the default (Ctrl+S in /model) joins a scope that is not empty -- the
 * session's, and settings.json's enabledModels when that holds patterns and none names the model (compared in any case). 0, -1 failed */
int pi_scope_add_default(pi_models *ms, const llm_model *to, cl_sys *sys, const char *dir);

#endif
