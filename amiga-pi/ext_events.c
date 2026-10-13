/* see ext_events.h */
#include <string.h>
#include "ext_events.h"
#include "ext_payload.h"

static int plain(pi_session *s, const char *type, jw *b)
{
    int stopped = 0;
    if (!b->oom)
        stopped = pi_exts_emit(&s->ext, type, b, 0, 0, 0);
    jw_free(b);
    return stopped;
}

static int cancel_apply(void *u, jv answer, jv after, jw *payload)
{
    (void)after;
    (void)payload;
    if (pi_ext_decide_cancel(answer)) {
        *(int *)u = 1;
        return 1;
    }
    return 0;
}

static int cancelled(pi_session *s, const char *type, jw *b)
{
    int cancel = 0;
    if (!b->oom)
        pi_exts_emit(&s->ext, type, b, 0, cancel_apply, &cancel);
    jw_free(b);
    return cancel;
}

/* resources_discover follows each session_start (agent-session.ts bindExtensions 3289) */
static void bound(void *u, const char *reason)
{
    pi_session *s = (pi_session *)u;
    jw b;
    if (!pi_exts_has(&s->ext, "resources_discover"))
        return;
    jw_init(&b);
    pi_ext_payload_resources(&b, s->ext.cwd ? s->ext.cwd : "", !strcmp(reason, "reload") ? "reload" : "startup");
    plain(s, "resources_discover", &b);
}

void pi_ev_attach(pi_session *s)
{
    s->ext.bound = bound;
    s->ext.bound_u = s;
}

void pi_ev_ui_prompt(pi_session *s, int start, const char *kind, const char *title)
{
    jw b;
    const char *type = start ? "ui_prompt_start" : "ui_prompt_end";
    if (!s || !pi_exts_has(&s->ext, type))
        return;
    jw_init(&b);
    pi_ext_payload_ui_prompt(&b, start, kind, title);
    plain(s, type, &b);
}

void pi_ev_model_select(pi_session *s, const llm_model *model, const llm_model *previous, const char *source)
{
    jw b;
    if (!s || !pi_exts_has(&s->ext, "model_select"))
        return;
    if (previous && !strcmp(previous->provider, model->provider) && !strcmp(previous->id, model->id))
        return;                 /* modelsAreEqual */
    jw_init(&b);
    pi_ext_payload_model_select(&b, model, previous, source);
    plain(s, "model_select", &b);
}

void pi_ev_thinking(pi_session *s, const char *level, const char *previous)
{
    jw b;
    if (!s || !pi_exts_has(&s->ext, "thinking_level_select"))
        return;
    jw_init(&b);
    pi_ext_payload_thinking(&b, level, previous);
    plain(s, "thinking_level_select", &b);
}

void pi_ev_session_info(pi_session *s, const char *name)
{
    jw b;
    if (!s || !pi_exts_has(&s->ext, "session_info_changed"))
        return;
    jw_init(&b);
    pi_ext_payload_session_info(&b, name);
    plain(s, "session_info_changed", &b);
}

void pi_ev_shutdown(pi_session *s, const char *reason, const char *target)
{
    jw b;
    if (!s || !pi_exts_has(&s->ext, "session_shutdown"))
        return;
    jw_init(&b);
    pi_ext_payload_shutdown(&b, reason, target);
    plain(s, "session_shutdown", &b);
}

int pi_ev_before_switch(pi_session *s, const char *reason, const char *target)
{
    jw b;
    if (!s || !pi_exts_has(&s->ext, "session_before_switch"))
        return 0;
    jw_init(&b);
    pi_ext_payload_before_switch(&b, reason, target);
    return cancelled(s, "session_before_switch", &b);
}

int pi_ev_before_fork(pi_session *s, const char *entry_id, const char *position)
{
    jw b;
    if (!s || !pi_exts_has(&s->ext, "session_before_fork"))
        return 0;
    jw_init(&b);
    pi_ext_payload_before_fork(&b, entry_id, position);
    return cancelled(s, "session_before_fork", &b);
}
