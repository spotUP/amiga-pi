/* see ext_events.h */
#include <stdlib.h>
#include <string.h>
#include "ext_events.h"
#include "ext_payload.h"
#include "claude/util.h"

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

/* ---- turn_end, agent_before_settle ---- */

typedef struct bnd {
    pi_session *s;
    int settle;
    const char *outcome;
    const char *head;           /* turn_end: the members before the ids, from "type" on; settle: from "type" on too */
    jw ids, ctx, entries;
    int cont;
} bnd;

static void bnd_payload(bnd *b, jw *p)
{
    jw_reset(p);
    jw_rawz(p, b->head);
    if (b->ids.n) {
        jw_rawz(p, ",");
        jw_raw(p, b->ids.p, b->ids.n);
    }
    jw_rawz(p, ",\"outcome\":");
    jw_strz(p, b->outcome);
    jw_rawz(p, ",\"entries\":");
    jw_raw(p, b->entries.n ? b->entries.p : "[]", b->entries.n ? b->entries.n : 2);
    jw_rawz(p, b->cont ? ",\"continue\":true" : ",\"continue\":false");
    jw_rawz(p, ",\"context\":");
    jw_raw(p, b->ctx.p, b->ctx.n);
    jw_rawz(p, "}");
}

static int bnd_apply(void *u, jv answer, jv after, jw *payload)
{
    bnd *b = (bnd *)u;
    jv v;
    (void)after;
    if (!answer.p)
        return 0;
    if (json_get(answer, "entries", &v) && json_type(v) == J_ARR) {
        jw_reset(&b->entries);
        jw_raw(&b->entries, v.p, v.n);
    }
    if (json_get(answer, "continue", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
        b->cont = json_type(v) == J_TRUE;
    bnd_payload(b, payload);
    return 0;
}

/* the drafts of the answer appended; the continuation honoured only when a request could be made (_reportInvalidBoundaryContinuation) */
static int bnd_finish(bnd *b, const char *type)
{
    pi_session *s = b->s;
    int go = b->cont;
    if (b->entries.n > 2 && s->draft) {
        jv arr, d;
        jit it;
        if (!json_parse(b->entries.p, b->entries.n, &arr)) {
            json_iter(arr, &it);
            while (json_next(&it, 0, &d))
                if (s->draft(s->boundary_u, s, d.p, d.n) < 0 && s->h.notice) {
                    char m[200];
                    cl_copy(m, "Extension \"<boundary>\" error: ", sizeof(m));
                    cl_cat(m, type, sizeof(m));
                    cl_cat(m, " draft entry not supported", sizeof(m));
                    s->h.notice(s->h.u, m);
                }
        }
    }
    if (go && s->boundary) {
        jw ctx;
        jw_init(&ctx);
        if (s->boundary(s->boundary_u, s, b->settle, 0, &ctx) == 0 && ctx.p && !strstr(ctx.p + (ctx.n > 24 ? ctx.n - 24 : 0), "\"canContinue\":true")) {
            char m[200];
            cl_copy(m, "Extension \"<boundary>\" error: ", sizeof(m));
            cl_cat(m, type, sizeof(m));
            cl_cat(m, " requested continuation without runnable model context", sizeof(m));
            if (s->h.notice)
                s->h.notice(s->h.u, m);
            go = 0;
        }
        jw_free(&ctx);
    }
    return go;
}

static const char *outcome_of(const pi_session *s)
{
    return s->stop == LS_ABORTED ? "aborted" : s->stop == LS_ERROR ? "error" : "completed";
}

int pi_ev_turn_end(pi_session *s, const char *message, long message_n, const char *results, long results_n)
{
    bnd b;
    jw p, head;
    int go = 0;
    if (!s || !pi_exts_has(&s->ext, "turn_end") || !s->boundary)
        return 0;
    memset(&b, 0, sizeof(b));
    b.s = s;
    b.outcome = outcome_of(s);
    jw_init(&b.ids);
    jw_init(&b.ctx);
    jw_init(&b.entries);
    jw_init(&p);
    jw_init(&head);
    if (s->boundary(s->boundary_u, s, 0, &b.ids, &b.ctx) == 0) {
        jw_rawz(&head, "{\"type\":\"turn_end\",\"turnIndex\":");
        jw_long(&head, s->ext.turn);
        jw_rawz(&head, ",\"message\":");
        jw_raw(&head, message ? message : "null", message ? message_n : 4);
        jw_rawz(&head, ",\"toolResults\":");
        jw_raw(&head, results ? results : "[]", results ? results_n : 2);
        b.head = head.p;
        bnd_payload(&b, &p);
        if (!p.oom && !head.oom) {
            pi_exts_emit(&s->ext, "turn_end", &p, 0, bnd_apply, &b);
            go = bnd_finish(&b, "turn_end");
        }
    }
    jw_free(&b.ids);
    jw_free(&b.ctx);
    jw_free(&b.entries);
    jw_free(&p);
    jw_free(&head);
    return go;
}

int pi_ev_before_settle(pi_session *s)
{
    bnd b;
    jw p, head;
    int go = 0;
    if (!s || !pi_exts_has(&s->ext, "agent_before_settle") || !s->boundary)
        return 0;
    memset(&b, 0, sizeof(b));
    b.s = s;
    b.settle = 1;
    b.outcome = outcome_of(s);
    jw_init(&b.ids);
    jw_init(&b.ctx);
    jw_init(&b.entries);
    jw_init(&p);
    jw_init(&head);
    if (s->boundary(s->boundary_u, s, 1, 0, &b.ctx) == 0) {
        jw_rawz(&head, "{\"type\":\"agent_before_settle\"");
        b.head = head.p;
        bnd_payload(&b, &p);
        if (!p.oom && !head.oom) {
            pi_exts_emit(&s->ext, "agent_before_settle", &p, 0, bnd_apply, &b);
            go = bnd_finish(&b, "agent_before_settle");
        }
    }
    jw_free(&b.ids);
    jw_free(&b.ctx);
    jw_free(&b.entries);
    jw_free(&p);
    jw_free(&head);
    return go;
}

/* ---- context, context_with_system ---- */

typedef struct cx {
    jw cur;                     /* the messages array the next handler is given */
    const char *type;
    int changed;
} cx;

static void cx_payload(cx *c, jw *p)
{
    jw_reset(p);
    jw_rawz(p, "{\"type\":");
    jw_strz(p, c->type);
    jw_rawz(p, ",\"messages\":");
    jw_raw(p, c->cur.p, c->cur.n);
    jw_rawz(p, "}");
}

static int cx_apply(void *u, jv answer, jv after, jw *payload)
{
    cx *c = (cx *)u;
    jv m;
    int take = 0;
    if (answer.p && json_get(answer, "messages", &m) && json_type(m) == J_ARR)
        take = 1;
    else if (after.p && json_get(after, "messages", &m) && json_type(m) == J_ARR)
        take = 1;
    if (take) {
        jw t;
        jw_init(&t);
        jw_raw(&t, m.p, m.n);               /* m points into the payload or the answer: copied before cur is rewritten */
        jw_reset(&c->cur);
        jw_raw(&c->cur, t.p, t.n);
        jw_free(&t);
        c->changed = 1;
        cx_payload(c, payload);
    }
    return 0;
}

int pi_ev_context(pi_session *s, jw *out)
{
    cx c;
    jw p;
    int i, any = 0;
    int has_c = pi_exts_has(&s->ext, "context"), has_w = pi_exts_has(&s->ext, "context_with_system");
    if (!has_c && !has_w)
        return 0;
    memset(&c, 0, sizeof(c));
    jw_init(&c.cur);
    jw_init(&p);
    if (has_c) {
        jw_rawz(&c.cur, "[");
        for (i = 0; i < s->nmsg; i++) {
            if (i)
                jw_rawz(&c.cur, ",");
            jw_raw(&c.cur, s->msg[i], s->msg_n[i]);
        }
        jw_rawz(&c.cur, "]");
        c.type = "context";
        cx_payload(&c, &p);
        pi_exts_emit(&s->ext, "context", &p, PI_EMIT_INPLACE, cx_apply, &c);
    }
    if (c.changed) {
        /* restoreSystemMessages: a changed conversation gets the prompt and tool declarations back in front of it */
        jw full;
        jw_init(&full);
        jw_rawz(&full, "[");
        for (i = 0; i < s->nsys; i++) {
            if (i)
                jw_rawz(&full, ",");
            jw_raw(&full, s->sys[i], s->sys_n[i]);
        }
        if (c.cur.n > 2) {
            if (s->nsys)
                jw_rawz(&full, ",");
            jw_raw(&full, c.cur.p + 1, c.cur.n - 2);        /* the members without the brackets */
        }
        jw_rawz(&full, "]");
        jw_reset(&c.cur);
        jw_raw(&c.cur, full.p, full.n);
        jw_free(&full);
        any = 1;
    } else if (has_w) {
        jw_reset(&c.cur);
        pi_messages_json(s, 0, 0, &c.cur);
    }
    if (has_w) {
        int was = c.changed;
        c.changed = 0;
        c.type = "context_with_system";
        if (!any) {
            jw_reset(&c.cur);
            pi_messages_json(s, 0, 0, &c.cur);
        }
        cx_payload(&c, &p);
        pi_exts_emit(&s->ext, "context_with_system", &p, PI_EMIT_INPLACE, cx_apply, &c);
        if (c.changed)
            any = 1;
        c.changed = was;
    }
    if (any && !c.cur.oom) {
        jw_reset(out);
        jw_raw(out, c.cur.p, c.cur.n);
    } else
        any = 0;
    jw_free(&c.cur);
    jw_free(&p);
    return any;
}

/* ---- before_agent_start ---- */

static int bas_apply(void *u, jv answer, jv after, jw *payload)
{
    pi_session *s = (pi_session *)u;
    jv m, v;
    (void)after;
    (void)payload;
    if (answer.p && json_get(answer, "message", &m) && json_type(m) == J_OBJ && s->nnext_custom < 8) {
        jw t;
        jv ct;
        jw_init(&t);
        jw_rawz(&t, "{\"customType\":");
        if (json_get(m, "customType", &ct))
            jw_raw(&t, ct.p, ct.n);
        else
            jw_rawz(&t, "\"\"");
        jw_rawz(&t, ",\"content\":");
        if (json_get(m, "content", &v) && json_type(v) != J_NULL)
            jw_raw(&t, v.p, v.n);
        else
            jw_rawz(&t, "[]");
        jw_rawz(&t, json_get(m, "display", &v) && json_type(v) == J_TRUE ? ",\"display\":true" : ",\"display\":false");
        if (json_get(m, "details", &v) && json_type(v) != J_NULL) {
            jw_rawz(&t, ",\"details\":");
            jw_raw(&t, v.p, v.n);
        }
        jw_rawz(&t, "}");
        if (!t.oom) {
            s->next_custom[s->nnext_custom] = (char *)malloc((size_t)t.n + 1);
            if (s->next_custom[s->nnext_custom]) {
                memcpy(s->next_custom[s->nnext_custom], t.p, (size_t)t.n);
                s->next_custom[s->nnext_custom++][t.n] = 0;
            }
        }
        jw_free(&t);
    }
    return 0;
}

void pi_ev_before_agent_start(pi_session *s, const char *prompt)
{
    jw p;
    int i;
    if (!s || !pi_exts_has(&s->ext, "before_agent_start"))
        return;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"before_agent_start\",\"prompt\":");
    jw_strz(&p, prompt);
    jw_rawz(&p, ",\"systemPrompt\":");
    jw_strz(&p, s->system ? s->system : "");
    jw_rawz(&p, ",\"systemPromptOptions\":{\"selectedTools\":[");
    for (i = 0; i < s->ntools; i++) {
        if (i)
            jw_rawz(&p, ",");
        jw_strz(&p, s->tools[i]->name);
    }
    jw_rawz(&p, "],\"hiddenTools\":[],\"toolSnippets\":{");
    for (i = 0; i < s->ntools; i++) {
        if (i)
            jw_rawz(&p, ",");
        jw_strz(&p, s->tools[i]->name);
        jw_rawz(&p, ":");
        jw_strz(&p, s->tools[i]->snippet ? s->tools[i]->snippet : "");
    }
    jw_rawz(&p, "},\"toolGuidelines\":{},\"promptGuidelines\":[],\"appendSystemPrompt\":\"\",\"sections\":{},\"cwd\":");
    jw_strz(&p, s->ext.cwd ? s->ext.cwd : "");
    jw_rawz(&p, ",\"contextFiles\":[],\"skills\":[]}}");
    if (!p.oom)
        pi_exts_emit(&s->ext, "before_agent_start", &p, 0, bas_apply, s);
    jw_free(&p);
}
