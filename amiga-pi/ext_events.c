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

/* resources_discover follows each session_start (agent-session.ts bindExtensions 3289); the paths the handlers return
 * ({skillPaths, promptPaths, themePaths}) go to the mode, which reads its skills and prompt templates again. Theme paths are
 * not read: a theme is chosen by name from the theme directories */
static int res_apply(void *u, jv answer, jv after, jw *payload)
{
    pi_session *s = (pi_session *)u;
    int k;
    (void)after;
    (void)payload;
    for (k = 0; answer.p && k < 2; k++) {
        jv arr, e;
        jit it;
        if (json_get(answer, k ? "promptPaths" : "skillPaths", &arr) && json_type(arr) == J_ARR) {
            json_iter(arr, &it);
            while (json_next(&it, 0, &e))
                if (json_type(e) == J_STR && s->ext.discovered) {
                    char path[300];
                    json_str(e, path, sizeof(path));
                    s->ext.discovered(s->ext.discovered_u, k ? "prompt" : "skill", path, s->ext.current);
                }
        }
    }
    return 0;
}

static void bound(void *u, const char *reason)
{
    pi_session *s = (pi_session *)u;
    jw b;
    if (!pi_exts_has(&s->ext, "resources_discover"))
        return;
    jw_init(&b);
    pi_ext_payload_resources(&b, s->ext.cwd ? s->ext.cwd : "", !strcmp(reason, "reload") ? "reload" : "startup");
    if (!b.oom)
        pi_exts_emit(&s->ext, "resources_discover", &b, 0, res_apply, s);
    jw_free(&b);
    if (s->ext.discovered)
        s->ext.discovered(s->ext.discovered_u, 0, 0, 0);
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
                if (s->draft(s->boundary_u, s, d.p, d.n) < 0)
                    pi_ev_error(s, "<boundary>", type, "Invalid boundary entries: this draft type is not supported");
        }
    }
    if (go && s->boundary) {
        jw ctx;
        jw_init(&ctx);
        if (s->boundary(s->boundary_u, s, b->settle, 0, &ctx) == 0 && ctx.p && !strstr(ctx.p + (ctx.n > 24 ? ctx.n - 24 : 0), "\"canContinue\":true")) {
            char m[200];
            cl_copy(m, type, sizeof(m));
            cl_cat(m, " requested continuation without runnable model context", sizeof(m));
            pi_ev_error(s, "<boundary>", type, m);
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
            if (s->alt[i])
                jw_raw(&c.cur, s->alt[i], s->alt_n[i]);        /* the AgentMessage, as pi's handlers see it */
            else
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

/* ---- user_bash ---- */

typedef struct ub {
    jw *text;
    int *code, *cancelled;
    int got;
} ub;

static int ub_apply(void *u, jv answer, jv after, jw *payload)
{
    ub *b = (ub *)u;
    jv r, v;
    (void)after;
    (void)payload;
    if (!answer.p || !json_get(answer, "result", &r) || json_type(r) != J_OBJ)
        return 0;
    jw_reset(b->text);
    if (json_get(r, "output", &v) && json_type(v) == J_STR) {
        long n;
        char *z = json_strdup(v, &n);
        if (z) {
            jw_raw(b->text, z, n);
            free(z);
        }
    }
    *b->code = json_get(r, "exitCode", &v) && json_type(v) == J_NUM ? (int)json_long(v, 0) : 0;
    *b->cancelled = json_get(r, "cancelled", &v) && json_type(v) == J_TRUE;
    b->got = 1;
    return 1;
}

int pi_ev_user_bash(pi_session *s, const char *command, int exclude, jw *text, int *code, int *cancelled)
{
    jw p;
    ub b;
    if (!s || !pi_exts_has(&s->ext, "user_bash"))
        return 0;
    b.text = text;
    b.code = code;
    b.cancelled = cancelled;
    b.got = 0;
    jw_init(&p);
    pi_ext_payload_user_bash(&p, command, exclude, s->ext.cwd ? s->ext.cwd : "");
    if (!p.oom)
        pi_exts_emit(&s->ext, "user_bash", &p, 0, ub_apply, &b);
    jw_free(&p);
    return b.got;
}

/* ---- compaction ---- */

static const char *cp_reason(int r)
{
    return r == 0 ? "manual" : r == 1 ? "threshold" : "overflow";
}

typedef struct cpb {
    int cancel, comp;
    jw *out;
} cpb;

static int cpb_apply(void *u, jv answer, jv after, jw *payload)
{
    cpb *b = (cpb *)u;
    jv v;
    (void)after;
    (void)payload;
    if (!answer.p)
        return 0;
    if (pi_ext_decide_cancel(answer)) {
        b->cancel = 1;
        return 1;
    }
    if (json_get(answer, "compaction", &v) && json_type(v) == J_OBJ) {
        jw_reset(b->out);
        jw_raw(b->out, v.p, v.n);
        b->comp = 1;                    /* the last override wins */
    }
    return 0;
}

int pi_ev_before_compact(pi_session *s, const char *prep, const char *branch, const char *instr, int reason, int will_retry,
                         jw *comp)
{
    jw p;
    cpb b;
    if (!s || !pi_exts_has(&s->ext, "session_before_compact"))
        return 0;
    b.cancel = b.comp = 0;
    b.out = comp;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"session_before_compact\",\"preparation\":");
    jw_rawz(&p, prep);
    jw_rawz(&p, ",\"branchEntries\":");
    jw_rawz(&p, branch);
    if (instr) {
        jw_rawz(&p, ",\"customInstructions\":");
        jw_strz(&p, instr);
    }
    jw_rawz(&p, ",\"reason\":");
    jw_strz(&p, cp_reason(reason));
    jw_rawz(&p, will_retry ? ",\"willRetry\":true" : ",\"willRetry\":false");
    jw_rawz(&p, ",\"signal\":{}}");
    if (!p.oom)
        pi_exts_emit(&s->ext, "session_before_compact", &p, 0, cpb_apply, &b);
    jw_free(&p);
    return b.cancel ? 1 : b.comp ? 2 : 0;
}

void pi_ev_compact(pi_session *s, const char *entry, long entry_n, int from_ext, int reason, int will_retry)
{
    jw p;
    if (!s || !pi_exts_has(&s->ext, "session_compact"))
        return;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"session_compact\",\"compactionEntry\":");
    jw_raw(&p, entry, entry_n);
    jw_rawz(&p, from_ext ? ",\"fromExtension\":true,\"reason\":" : ",\"fromExtension\":false,\"reason\":");
    jw_strz(&p, cp_reason(reason));
    jw_rawz(&p, will_retry ? ",\"willRetry\":true}" : ",\"willRetry\":false}");
    plain(s, "session_compact", &p);
}

void pi_ev_compact_failed(pi_session *s, int reason, const char *error, int aborted, int will_retry, int from_ext)
{
    jw p;
    if (!s || !pi_exts_has(&s->ext, "session_compact_failed"))
        return;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"session_compact_failed\",\"reason\":");
    jw_strz(&p, cp_reason(reason));
    if (error && !aborted) {
        jw_rawz(&p, ",\"errorMessage\":");
        jw_strz(&p, error);
    }
    jw_rawz(&p, aborted ? ",\"aborted\":true" : ",\"aborted\":false");
    jw_rawz(&p, will_retry ? ",\"willRetry\":true" : ",\"willRetry\":false");
    jw_rawz(&p, from_ext ? ",\"fromExtension\":true}" : ",\"fromExtension\":false}");
    plain(s, "session_compact_failed", &p);
}

/* ---- session_before_tree, session_tree ---- */

void pi_tree_dec_init(pi_tree_dec *d)
{
    memset(d, 0, sizeof(*d));
    jw_init(&d->summary);
    jw_init(&d->details);
    jw_init(&d->usage);
    jw_init(&d->instr);
    jw_init(&d->label);
}

void pi_tree_dec_free(pi_tree_dec *d)
{
    jw_free(&d->summary);
    jw_free(&d->details);
    jw_free(&d->usage);
    jw_free(&d->instr);
    jw_free(&d->label);
}

static int tree_apply(void *u, jv answer, jv after, jw *payload)
{
    pi_tree_dec *d = (pi_tree_dec *)u;
    jv v, x;
    (void)after;
    (void)payload;
    if (!answer.p)
        return 0;
    if (pi_ext_decide_cancel(answer)) {
        d->cancel = 1;
        return 1;
    }
    if (json_get(answer, "summary", &v) && json_type(v) == J_OBJ && json_get(v, "summary", &x) && json_type(x) == J_STR) {
        long n;
        char *z = json_strdup(x, &n);
        jw_reset(&d->summary);
        if (z) {
            jw_raw(&d->summary, z, n);
            free(z);
        }
        d->has_summary = 1;
        jw_reset(&d->details);
        if (json_get(v, "details", &x) && json_type(x) != J_NULL)
            jw_raw(&d->details, x.p, x.n);
        jw_reset(&d->usage);
        if (json_get(v, "usage", &x) && json_type(x) == J_OBJ)
            jw_raw(&d->usage, x.p, x.n);
    }
    if (json_get(answer, "customInstructions", &v) && json_type(v) == J_STR) {
        long n;
        char *z = json_strdup(v, &n);
        jw_reset(&d->instr);
        if (z) {
            jw_raw(&d->instr, z, n);
            free(z);
        }
        d->has_instr = 1;
    }
    if (json_get(answer, "replaceInstructions", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE)) {
        d->has_replace = 1;
        d->replace = json_type(v) == J_TRUE;
    }
    if (json_get(answer, "label", &v) && json_type(v) == J_STR) {
        long n;
        char *z = json_strdup(v, &n);
        jw_reset(&d->label);
        if (z) {
            jw_raw(&d->label, z, n);
            free(z);
        }
        d->has_label = 1;
    }
    return 0;
}

int pi_ev_before_tree(pi_session *s, const char *target, const char *old_leaf, const char *ancestor, const char *entries,
                      int summarize, const char *instr, int replace_set, int replace, const char *label, pi_tree_dec *dec)
{
    jw p;
    if (!s || !pi_exts_has(&s->ext, "session_before_tree"))
        return 0;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"session_before_tree\",\"preparation\":{\"targetId\":");
    jw_strz(&p, target);
    jw_rawz(&p, ",\"oldLeafId\":");
    if (old_leaf)
        jw_strz(&p, old_leaf);
    else
        jw_rawz(&p, "null");
    jw_rawz(&p, ",\"commonAncestorId\":");
    if (ancestor)
        jw_strz(&p, ancestor);
    else
        jw_rawz(&p, "null");
    jw_rawz(&p, ",\"entriesToSummarize\":");
    jw_rawz(&p, entries);
    jw_rawz(&p, summarize ? ",\"userWantsSummary\":true" : ",\"userWantsSummary\":false");
    if (instr) {
        jw_rawz(&p, ",\"customInstructions\":");
        jw_strz(&p, instr);
    }
    if (replace_set)
        jw_rawz(&p, replace ? ",\"replaceInstructions\":true" : ",\"replaceInstructions\":false");
    if (label) {
        jw_rawz(&p, ",\"label\":");
        jw_strz(&p, label);
    }
    jw_rawz(&p, "},\"signal\":{}}");
    if (!p.oom)
        pi_exts_emit(&s->ext, "session_before_tree", &p, 0, tree_apply, dec);
    jw_free(&p);
    return dec->cancel;
}

void pi_ev_tree(pi_session *s, const char *new_leaf, const char *old_leaf, const char *summary_entry, long summary_n, int from_ext)
{
    jw p;
    if (!s || !pi_exts_has(&s->ext, "session_tree"))
        return;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"session_tree\",\"newLeafId\":");
    if (new_leaf)
        jw_strz(&p, new_leaf);
    else
        jw_rawz(&p, "null");
    jw_rawz(&p, ",\"oldLeafId\":");
    if (old_leaf)
        jw_strz(&p, old_leaf);
    else
        jw_rawz(&p, "null");
    if (summary_entry) {
        jw_rawz(&p, ",\"summaryEntry\":");
        jw_raw(&p, summary_entry, summary_n);
        jw_rawz(&p, from_ext ? ",\"fromExtension\":true" : ",\"fromExtension\":false");
    }
    jw_rawz(&p, "}");
    plain(s, "session_tree", &p);
}

/* ---- message_end ---- */

typedef struct mend {
    pi_session *s;
    char role[24];
    jw cur;
    int changed;
} mend;

static void mend_payload(mend *m, jw *p)
{
    jw_reset(p);
    jw_rawz(p, "{\"type\":\"message_end\",\"message\":");
    jw_raw(p, m->cur.p, m->cur.n);
    jw_rawz(p, "}");
}

static int mend_apply(void *u, jv answer, jv after, jw *payload)
{
    mend *m = (mend *)u;
    jv v, r;
    char role[24];
    (void)after;
    if (!answer.p || !json_get(answer, "message", &v) || json_type(v) != J_OBJ)
        return 0;
    role[0] = 0;
    if (json_get(v, "role", &r) && json_type(r) == J_STR)
        json_str(r, role, sizeof(role));
    if (strcmp(role, m->role)) {
        pi_ev_error(m->s, m->s->ext.current && m->s->ext.current->path ? m->s->ext.current->path : "", "message_end",
                    "message_end handlers must return a message with the same role");
        return 0;
    }
    {
        /* a handler's message back from a hook command or a Lua table has its members sorted: they go back in the order of the
         * message they replace, as a JavaScript object keeps them */
        jw t;
        jv was;
        jw_init(&t);
        if (!json_parse(m->cur.p, m->cur.n, &was) && json_type(was) == J_OBJ)
            pi_ext_reorder(was, v, &t);
        else
            jw_raw(&t, v.p, v.n);
        jw_reset(&m->cur);
        jw_raw(&m->cur, t.p, t.n);
        jw_free(&t);
    }
    m->changed = 1;
    mend_payload(m, payload);
    return 0;
}

int pi_ev_message_end(pi_session *s, const char *msg, long msg_n, jw *out)
{
    mend m;
    jw p;
    jv v, r;
    int changed;
    if (!s || !pi_exts_has(&s->ext, "message_end"))
        return 0;
    memset(&m, 0, sizeof(m));
    m.s = s;
    jw_init(&m.cur);
    jw_raw(&m.cur, msg, msg_n);
    if (!json_parse(msg, msg_n, &v) && json_get(v, "role", &r) && json_type(r) == J_STR)
        json_str(r, m.role, sizeof(m.role));
    jw_init(&p);
    mend_payload(&m, &p);
    if (!p.oom)
        pi_exts_emit(&s->ext, "message_end", &p, 0, mend_apply, &m);
    changed = m.changed && !m.cur.oom;
    if (changed) {
        jw_reset(out);
        jw_raw(out, m.cur.p, m.cur.n);
    }
    jw_free(&m.cur);
    jw_free(&p);
    return changed;
}

void pi_ev_error(pi_session *s, const char *path, const char *event, const char *message)
{
    char m[900];
    if (s->ext.report) {
        s->ext.report(s->ext.report_u, path, event, message);
        return;
    }
    cl_copy(m, "Extension \"", sizeof(m));
    cl_cat(m, path, sizeof(m));
    cl_cat(m, "\" error: ", sizeof(m));
    cl_cat(m, message, sizeof(m));
    if (s->h.notice)
        s->h.notice(s->h.u, m);
}

/* ---- the provider events ---- */

/* a handler's answer for before_provider_request is the payload itself (runner.ts emitBeforeProviderRequest: a result that is not undefined
 * replaces it for the next handler and for the request) */
static int tap_payload_apply(void *u, jv answer, jv after, jw *payload)
{
    jw *cur = (jw *)u;
    (void)after;
    if (answer.p && json_type(answer) == J_OBJ) {
        jw t;
        jv was;
        jw_init(&t);
        if (!json_parse(cur->p, cur->n, &was) && json_type(was) == J_OBJ)
            pi_ext_reorder(was, answer, &t);       /* the replacement keeps the body's member order */
        else
            jw_raw(&t, answer.p, answer.n);
        jw_reset(cur);
        jw_raw(cur, t.p, t.n);
        jw_free(&t);
        jw_reset(payload);
        jw_rawz(payload, "{\"type\":\"before_provider_request\",\"payload\":");
        jw_raw(payload, cur->p, cur->n);
        jw_rawz(payload, "}");
    }
    return 0;
}

static int tap_payload(void *u, const llm_model *m, jw *body)
{
    pi_session *s = (pi_session *)u;
    jw p, cur;
    (void)m;
    if (!pi_exts_has(&s->ext, "before_provider_request"))
        return 0;
    jw_init(&p);
    jw_init(&cur);
    jw_raw(&cur, body->p, body->n);
    jw_rawz(&p, "{\"type\":\"before_provider_request\",\"payload\":");
    jw_raw(&p, body->p, body->n);
    jw_rawz(&p, "}");
    if (!p.oom && !cur.oom) {
        pi_exts_emit(&s->ext, "before_provider_request", &p, 0, tap_payload_apply, &cur);
        if (!cur.oom && (cur.n != body->n || memcmp(cur.p, body->p, (size_t)cur.n))) {
            jw_reset(body);
            jw_raw(body, cur.p, cur.n);
        }
    }
    jw_free(&p);
    jw_free(&cur);
    return 0;
}

/* "Name: value\r\n" lines as a JSON object / back */
static void lines_to_json(const jw *lines, jw *o)
{
    const char *p = lines->p ? lines->p : "";
    long n = lines->n, i = 0;
    int first = 1;
    jw_rawz(o, "{");
    while (i < n) {
        const char *e = memchr(p + i, '\n', (size_t)(n - i));
        long end = e ? (long)(e - p) : n, ln = end - i, c;
        const char *colon;
        if (ln > 0 && p[i + ln - 1] == '\r')
            ln--;
        colon = ln > 0 ? memchr(p + i, ':', (size_t)ln) : 0;
        if (colon) {
            c = (long)(colon - (p + i));
            if (!first)
                jw_rawz(o, ",");
            first = 0;
            jw_str(o, p + i, c);
            jw_rawz(o, ":");
            {
                const char *v = colon + 1;
                long vl = ln - c - 1;
                while (vl > 0 && *v == ' ') {
                    v++;
                    vl--;
                }
                jw_str(o, v, vl);
            }
        }
        i = end + 1;
    }
    jw_rawz(o, "}");
}

static void json_to_lines(jv obj, jw *lines)
{
    jit it;
    jv k, v;
    json_iter(obj, &it);
    while (json_next(&it, &k, &v)) {
        char name[200];
        long vn;
        char *val;
        if (json_type(v) == J_NULL)
            continue;                                   /* a null value deletes the header (types.ts ProviderHeaders) */
        json_str(k, name, sizeof(name));
        val = json_type(v) == J_STR ? json_strdup(v, &vn) : 0;
        if (!val) {
            val = (char *)malloc((size_t)v.n + 1);
            if (!val)
                continue;
            memcpy(val, v.p, (size_t)v.n);
            vn = v.n;
        }
        jw_rawz(lines, name);
        jw_rawz(lines, ": ");
        jw_raw(lines, val, vn);
        jw_rawz(lines, "\r\n");
        free(val);
    }
}

typedef struct taph {
    jw *lines;
    jw cur;                     /* the headers object the next handler gets */
} taph;

static void taph_payload(taph *t, jw *p)
{
    jw_reset(p);
    jw_rawz(p, "{\"type\":\"before_provider_headers\",\"headers\":");
    jw_raw(p, t->cur.p, t->cur.n);
    jw_rawz(p, "}");
}

static int taph_apply(void *u, jv answer, jv after, jw *payload)
{
    taph *t = (taph *)u;
    jv h;
    /* handlers mutate `headers` in place and what they return is ignored (runner.ts emitBeforeProviderHeaders) */
    if (after.p && json_get(after, "headers", &h) && json_type(h) == J_OBJ) {
        jw n;
        jv was;
        jw_init(&n);
        if (!json_parse(t->cur.p, t->cur.n, &was) && json_type(was) == J_OBJ)
            pi_ext_reorder(was, h, &n);
        else
            jw_raw(&n, h.p, h.n);
        jw_reset(&t->cur);
        jw_raw(&t->cur, n.p, n.n);
        jw_free(&n);
        taph_payload(t, payload);
    }
    (void)answer;
    return 0;
}

static int tap_headers(void *u, const llm_model *m, jw *lines)
{
    pi_session *s = (pi_session *)u;
    taph t;
    jw p;
    jv obj;
    (void)m;
    if (!pi_exts_has(&s->ext, "before_provider_headers"))
        return 0;
    t.lines = lines;
    jw_init(&t.cur);
    lines_to_json(lines, &t.cur);
    jw_init(&p);
    taph_payload(&t, &p);
    if (!p.oom)
        pi_exts_emit(&s->ext, "before_provider_headers", &p, PI_EMIT_INPLACE, taph_apply, &t);
    if (!t.cur.oom && !json_parse(t.cur.p, t.cur.n, &obj) && json_type(obj) == J_OBJ) {
        jw_reset(lines);
        json_to_lines(obj, lines);
    }
    jw_free(&p);
    jw_free(&t.cur);
    return 0;
}

static void tap_response(void *u, const llm_model *m, int status, const char *headers, long hn)
{
    pi_session *s = (pi_session *)u;
    jw p;
    (void)m;
    if (!pi_exts_has(&s->ext, "after_provider_response"))
        return;
    jw_init(&p);
    jw_rawz(&p, "{\"type\":\"after_provider_response\",\"status\":");
    jw_long(&p, status);
    jw_rawz(&p, ",\"headers\":");
    jw_raw(&p, headers, hn);
    jw_rawz(&p, "}");
    plain(s, "after_provider_response", &p);
}

static void tap_stream(void *u, const llm_model *m, const char *data, long n)
{
    pi_session *s = (pi_session *)u;
    jw p;
    if (!pi_exts_has(&s->ext, "provider_stream_event"))
        return;
    jw_init(&p);
    jw_rawz(&p, "{\"data\":");
    jw_raw(&p, data, n);
    jw_rawz(&p, ",\"type\":\"provider_stream_event\",\"provider\":");
    jw_strz(&p, m->provider);
    jw_rawz(&p, ",\"api\":");
    jw_strz(&p, m->api);
    jw_rawz(&p, ",\"model\":");
    jw_strz(&p, m->id);
    jw_rawz(&p, "}");
    plain(s, "provider_stream_event", &p);
}

const llm_tap *pi_ev_tap(pi_session *s, llm_tap *out)
{
    int rq = pi_exts_has(&s->ext, "before_provider_request"), hd = pi_exts_has(&s->ext, "before_provider_headers"),
        rs = pi_exts_has(&s->ext, "after_provider_response"), st = pi_exts_has(&s->ext, "provider_stream_event");
    if (!rq && !hd && !rs && !st)
        return 0;
    memset(out, 0, sizeof(*out));
    out->u = s;
    out->payload = rq ? tap_payload : 0;
    out->headers = hd ? tap_headers : 0;
    out->response = rs ? tap_response : 0;
    out->stream = st ? tap_stream : 0;
    return out;
}
