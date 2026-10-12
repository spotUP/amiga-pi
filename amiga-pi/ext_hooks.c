/* see ext_hooks.h */
#include <stdlib.h>
#include <string.h>
#include "ext_hooks.h"
#include "mode_json.h"
#include "claude/util.h"

long pi_hook_runs;

/* one dispatch: the event's payload (rebuilt before each hook, so a hook sees what the one before it changed) and
 * what the answers did */
typedef struct hk_ctx {
    pi_hooks *k;
    jw body;
    int (*build)(struct hk_ctx *c);     /* the payload into body (reset first); 0 when there is none */
    const char *id, *name;              /* tool_call, tool_result */
    jw *args;                           /* the call's arguments: tool_call rewrites them */
    const char *args_p;                 /* tool_result: the arguments */
    long args_n;
    jw *content;                        /* tool_result */
    int *is_error;
    jw *text;                           /* input */
    const char *source, *streaming;
    char *reason;                       /* tool_call: the block's reason out */
    long cap;
    int blocked, handled;
} hk_ctx;

static const char *hk_body(void *u, long *n)
{
    hk_ctx *c = (hk_ctx *)u;
    jw_reset(&c->body);
    if (!c->build(c) || c->body.oom)
        return 0;
    jw_rawz(&c->body, "\n");
    *n = c->body.n;
    return c->body.p;
}

/* why a hook did not answer: the text for a tool_call's block and the notice of the others */
static void failure(const cl_hook *kk, int st, long rc, const char *out, long n, cl_sys *sys, char *m, long cap)
{
    char num[16];
    cl_copy(m, "Hook \"", cap);
    cl_cat(m, kk->cmd, cap);
    if (st < 0) {
        cl_cat(m, st == SYS_BREAK ? "\" was stopped." : st == SYS_TIMEOUT ? "\" ran out of time." : "\" did not run: ", cap);
        if (st == -1)
            cl_cat(m, sys->err(sys->u), cap);
        return;
    }
    cl_cat(m, "\" failed with ", cap);
    cl_ltoa(rc, num);
    cl_cat(m, num, cap);
    if (n) {
        jw t;
        jw_init(&t);
        jw_rawz(&t, ": ");
        jw_raw(&t, out, n > 200 ? 200 : n);
        cl_cat(m, t.p ? t.p : "", cap);
        jw_free(&t);
    } else
        cl_cat(m, ".", cap);
}

static void told(pi_hooks *k, const char *m)
{
    k->errors++;
    if (k->s->h.notice)
        k->s->h.notice(k->s->h.u, m);
}

/* the hook's output as the JSON object it answered, 0 when it said nothing or not an object */
static int answer(const char *out, long n, jv *obj)
{
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t'))
        n--;
    return n > 0 && json_parse(out, n, obj) == 0 && json_type(*obj) == J_OBJ;
}

/* the answer of a hook that failed, or ran: 1 when it did not answer (rc, status) */
static int bad(const hk_ctx *c, int st, long rc)
{
    (void)c;
    return st < 0 || rc != 0;
}

/* ---- the events ---- */

static int hk_of(int type)
{
    switch (type) {
    case PE_AGENT_START: return HK_PI_AGENT_START;
    case PE_TURN_START: return HK_PI_TURN_START;
    case PE_MESSAGE_START: return HK_PI_MESSAGE_START;
    case PE_MESSAGE_END: return HK_PI_MESSAGE_END;
    case PE_TOOL_START: return HK_PI_TOOL_EXEC_START;
    case PE_TOOL_UPDATE: return HK_PI_TOOL_EXEC_UPDATE;
    case PE_TOOL_END: return HK_PI_TOOL_EXEC_END;
    case PE_AGENT_END: return HK_PI_AGENT_END;
    case PE_AGENT_SETTLED: return HK_PI_AGENT_SETTLED;
    }
    return -1;
}

typedef struct hk_ev {
    hk_ctx c;
    const pi_event *e;
} hk_ev;

/* pi's payload of an agent event: the JSON mode's record (mode_json.c, the same shapes) except for turn_start (adds
 * turnIndex and timestamp) and agent_end (no willRetry: AgentEndEvent is {type, messages}) */
static int build_event(hk_ctx *c)
{
    hk_ev *v = (hk_ev *)c;
    const pi_event *e = v->e;
    pi_session *s = c->k->s;
    if (e->type == PE_TURN_START) {
        llm_ts t;
        t.sec = 0;
        t.ms = 0;
        if (s->now)
            t = s->now(s->h.u);
        jw_rawz(&c->body, "{\"type\":\"turn_start\",\"turnIndex\":");
        jw_long(&c->body, c->k->turn);
        jw_rawz(&c->body, ",\"timestamp\":");
        llm_w_ts(&c->body, t);                  /* the 13-digit milliseconds, in the 32 bits of a long too */
        jw_rawz(&c->body, "}");
        return 1;
    }
    if (e->type == PE_AGENT_END) {
        jw_rawz(&c->body, "{\"type\":\"agent_end\",\"messages\":");
        jw_raw(&c->body, e->result ? e->result : "[]", e->result ? e->result_n : 2);
        jw_rawz(&c->body, "}");
        return 1;
    }
    return pi_json_event(s, e, &c->body);
}

static int ev_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    if (bad(c, st, rc)) {
        char m[400];
        failure(kk, st, rc, out, n, c->k->h.sys, m, sizeof(m));
        told(c->k, m);
    }
    return 0;
}

static void hk_event(void *u, const pi_event *e)
{
    pi_hooks *k = (pi_hooks *)u;
    int ev = hk_of(e->type);
    if (e->type == PE_AGENT_START)
        k->turn = 0;
    if (ev >= 0 && hooks_any(&k->h, ev, e->tool_name ? e->tool_name : "")) {
        hk_ev v;
        memset(&v, 0, sizeof(v));
        v.c.k = k;
        v.c.build = build_event;
        v.e = e;
        jw_init(&v.c.body);
        pi_hook_runs += hooks_run_raw(&k->h, ev, e->tool_name ? e->tool_name : "", hk_body, ev_done, &v);
        jw_free(&v.c.body);
    }
    if (e->type == PE_TURN_END)
        k->turn++;
}

/* ---- tool_call ---- */

static int build_call(hk_ctx *c)
{
    jw_rawz(&c->body, "{\"type\":\"tool_call\",\"toolName\":");
    jw_strz(&c->body, c->name);
    jw_rawz(&c->body, ",\"toolCallId\":");
    jw_strz(&c->body, c->id);
    jw_rawz(&c->body, ",\"input\":");
    jw_raw(&c->body, c->args->p ? c->args->p : "{}", c->args->p ? c->args->n : 2);
    jw_rawz(&c->body, "}");
    return 1;
}

static int call_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    jv o, x;
    if (bad(c, st, rc)) {
        /* pi: a handler that throws blocks the call with the error's message */
        failure(kk, st, rc, out, n, c->k->h.sys, c->reason, c->cap);
        c->blocked = 1;
        return 1;
    }
    if (!answer(out, n, &o))
        return 0;
    if (json_get(o, "input", &x) && json_type(x) == J_OBJ) {
        jw_reset(c->args);
        jw_raw(c->args, x.p, x.n);
    }
    if (json_get(o, "block", &x) && json_type(x) == J_TRUE) {
        c->reason[0] = 0;
        if (json_get(o, "reason", &x) && json_type(x) == J_STR)
            json_str(x, c->reason, c->cap);
        c->blocked = 1;
        return 1;
    }
    return 0;
}

static int hk_tool_call(void *u, const char *id, const char *name, jw *args, char *reason, long cap)
{
    pi_hooks *k = (pi_hooks *)u;
    hk_ctx c;
    if (!hooks_any(&k->h, HK_PI_TOOL_CALL, name))
        return 0;
    memset(&c, 0, sizeof(c));
    c.k = k;
    c.build = build_call;
    c.id = id;
    c.name = name;
    c.args = args;
    c.reason = reason;
    c.cap = cap;
    jw_init(&c.body);
    pi_hook_runs += hooks_run_raw(&k->h, HK_PI_TOOL_CALL, name, hk_body, call_done, &c);
    jw_free(&c.body);
    return c.blocked;
}

/* ---- tool_result ---- */

static int build_result(hk_ctx *c)
{
    const pi_exts *x = &c->k->s->ext;
    int details = x->details && x->details->n, structured = x->structured && x->structured->n;
    jw_rawz(&c->body, "{\"type\":\"tool_result\",\"toolName\":");
    jw_strz(&c->body, c->name);
    jw_rawz(&c->body, ",\"toolCallId\":");
    jw_strz(&c->body, c->id);
    jw_rawz(&c->body, ",\"input\":");
    jw_raw(&c->body, c->args_p && c->args_n ? c->args_p : "{}", c->args_p && c->args_n ? c->args_n : 2);
    jw_rawz(&c->body, ",\"content\":");
    jw_raw(&c->body, c->content->p ? c->content->p : "[]", c->content->p ? c->content->n : 2);
    if (details) {
        jw_rawz(&c->body, ",\"details\":");
        jw_raw(&c->body, x->details->p, x->details->n);
    } else if (x->thrown && !structured)
        jw_rawz(&c->body, ",\"details\":{}");      /* a thrown error's result has details {} (mode_json.c) */
    if (structured) {
        jw_rawz(&c->body, ",\"structuredContent\":");
        jw_raw(&c->body, x->structured->p, x->structured->n);
    }
    jw_rawz(&c->body, *c->is_error ? ",\"isError\":true}" : ",\"isError\":false}");
    return 1;
}

static int result_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    pi_exts *x = &c->k->s->ext;
    jv o, v;
    if (bad(c, st, rc)) {
        char m[400];
        failure(kk, st, rc, out, n, c->k->h.sys, m, sizeof(m));
        told(c->k, m);
        return 0;
    }
    if (!answer(out, n, &o))
        return 0;
    if (json_get(o, "content", &v) && json_type(v) == J_ARR) {
        jw_reset(c->content);
        jw_raw(c->content, v.p, v.n);
        /* pi: structured content that is not replaced along with the content may no longer match it */
        if (x->structured && !json_get(o, "structuredContent", &v))
            jw_reset(x->structured);
    }
    if (json_get(o, "details", &v) && json_type(v) != J_NULL && x->details) {
        jw_reset(x->details);
        jw_raw(x->details, v.p, v.n);
    }
    if (json_get(o, "structuredContent", &v) && json_type(v) != J_NULL && x->structured) {
        jw_reset(x->structured);
        jw_raw(x->structured, v.p, v.n);
    }
    if (json_get(o, "isError", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
        *c->is_error = json_type(v) == J_TRUE;
    return 0;
}

static void hk_tool_result(void *u, const char *id, const char *name, const char *args, long an, jw *content,
                           int *is_error)
{
    pi_hooks *k = (pi_hooks *)u;
    hk_ctx c;
    if (!hooks_any(&k->h, HK_PI_TOOL_RESULT, name))
        return;
    memset(&c, 0, sizeof(c));
    c.k = k;
    c.build = build_result;
    c.id = id;
    c.name = name;
    c.args_p = args;
    c.args_n = an;
    c.content = content;
    c.is_error = is_error;
    jw_init(&c.body);
    pi_hook_runs += hooks_run_raw(&k->h, HK_PI_TOOL_RESULT, name, hk_body, result_done, &c);
    jw_free(&c.body);
}

/* ---- input ---- */

static int build_input(hk_ctx *c)
{
    jw_rawz(&c->body, "{\"type\":\"input\",\"text\":");
    jw_str(&c->body, c->text->p ? c->text->p : "", c->text->p ? c->text->n : 0);
    jw_rawz(&c->body, ",\"source\":");
    jw_strz(&c->body, c->source);
    if (c->streaming) {
        jw_rawz(&c->body, ",\"streamingBehavior\":");
        jw_strz(&c->body, c->streaming);
    }
    jw_rawz(&c->body, "}");
    return 1;
}

static int input_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    jv o, v;
    char *t;
    long tn;
    if (bad(c, st, rc)) {
        char m[400];
        failure(kk, st, rc, out, n, c->k->h.sys, m, sizeof(m));
        told(c->k, m);
        return 0;
    }
    if (!answer(out, n, &o) || !json_get(o, "action", &v))
        return 0;
    if (json_streq(v, "handled")) {
        c->handled = 1;
        return 1;
    }
    if (json_streq(v, "transform") && json_get(o, "text", &v) && (t = json_strdup(v, &tn)) != 0) {
        jw_reset(c->text);
        jw_raw(c->text, t, tn);
        free(t);
    }
    return 0;
}

static int hk_input(void *u, const char *source, const char *streaming, jw *text)
{
    pi_hooks *k = (pi_hooks *)u;
    hk_ctx c;
    if (!hooks_any(&k->h, HK_PI_INPUT, ""))
        return 0;
    memset(&c, 0, sizeof(c));
    c.k = k;
    c.build = build_input;
    c.text = text;
    c.source = source;
    c.streaming = streaming;
    jw_init(&c.body);
    pi_hook_runs += hooks_run_raw(&k->h, HK_PI_INPUT, "", hk_body, input_done, &c);
    jw_free(&c.body);
    return c.handled;
}

/* ---- session_start ---- */

static int build_start(hk_ctx *c)
{
    jw_rawz(&c->body, "{\"type\":\"session_start\",\"reason\":");
    jw_strz(&c->body, c->source);
    if (c->name) {
        jw_rawz(&c->body, ",\"previousSessionFile\":");
        jw_strz(&c->body, c->name);
    }
    jw_rawz(&c->body, "}");
    return 1;
}

static void hk_session_start(void *u, const char *reason, const char *prev)
{
    pi_hooks *k = (pi_hooks *)u;
    hk_ctx c;
    if (!hooks_any(&k->h, HK_PI_SESSION_START, ""))
        return;
    memset(&c, 0, sizeof(c));
    c.k = k;
    c.build = build_start;
    c.source = reason;
    c.name = prev;
    jw_init(&c.body);
    pi_hook_runs += hooks_run_raw(&k->h, HK_PI_SESSION_START, "", hk_body, ev_done, &c);
    jw_free(&c.body);
}

/* ---- setup ---- */

void pi_hooks_init(pi_hooks *k, pi_session *s, cl_sys *sys, const char *tmp, const char *cwd)
{
    memset(k, 0, sizeof(*k));
    k->s = s;
    cfg_init(&k->cfg);
    k->h.cfg = &k->cfg;
    k->h.sys = sys;
    k->h.tmp = tmp;
    k->h.cwd = cwd;
    k->h.session_id = s->session_id;
    k->ext.name = "hook-commands";
    k->ext.u = k;
    k->ext.event = hk_event;
    k->ext.tool_call = hk_tool_call;
    k->ext.tool_result = hk_tool_result;
    k->ext.input = hk_input;
    k->ext.session_start = hk_session_start;
}

void pi_hooks_free(pi_hooks *k)
{
    cfg_free(&k->cfg);
}

int pi_hooks_settings(pi_hooks *k, const char *json, long n, const char *name)
{
    return cfg_merge(&k->cfg, CFG_USER, json, n, name);
}

int pi_hooks_any(const pi_hooks *k)
{
    return !k->cfg.no_hooks && k->cfg.nhooks > 0;
}

int pi_hooks_add(pi_hooks *k)
{
    if (!pi_hooks_any(k))
        return 0;
    return pi_ext_add(&k->s->ext, &k->ext);
}
