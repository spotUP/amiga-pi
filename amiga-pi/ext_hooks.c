/* see ext_hooks.h */
#include <stdlib.h>
#include <string.h>
#include "ext_hooks.h"
#include "ext_payload.h"
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

static int build_event(hk_ctx *c)
{
    hk_ev *v = (hk_ev *)c;
    return pi_ext_payload_event(c->k->s, v->e, &c->body);
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
}

/* ---- tool_call ---- */

static int build_call(hk_ctx *c)
{
    pi_ext_payload_call(&c->body, c->name, c->id, c->args);
    return 1;
}

static int call_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    jv o;
    if (bad(c, st, rc)) {
        /* pi: a handler that throws blocks the call with the error's message */
        failure(kk, st, rc, out, n, c->k->h.sys, c->reason, c->cap);
        c->blocked = 1;
        return 1;
    }
    if (!answer(out, n, &o))
        return 0;
    if (pi_ext_decide_call(o, c->args, c->reason, c->cap)) {
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
    pi_ext_payload_result(&c->body, &c->k->s->ext, c->name, c->id, c->args_p, c->args_n, c->content, *c->is_error);
    return 1;
}

static int result_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    pi_exts *x = &c->k->s->ext;
    jv o;
    if (bad(c, st, rc)) {
        char m[400];
        failure(kk, st, rc, out, n, c->k->h.sys, m, sizeof(m));
        told(c->k, m);
        return 0;
    }
    if (!answer(out, n, &o))
        return 0;
    pi_ext_decide_result(o, x, c->content, c->is_error);
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
    pi_ext_payload_input(&c->body, c->text, c->source, c->streaming);
    return 1;
}

static int input_done(void *u, const cl_hook *kk, int st, long rc, const char *out, long n)
{
    hk_ctx *c = (hk_ctx *)u;
    jv o;
    if (bad(c, st, rc)) {
        char m[400];
        failure(kk, st, rc, out, n, c->k->h.sys, m, sizeof(m));
        told(c->k, m);
        return 0;
    }
    if (!answer(out, n, &o))
        return 0;
    if (pi_ext_decide_input(o, c->text)) {
        c->handled = 1;
        return 1;
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
    pi_ext_payload_start(&c->body, c->source, c->name);
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
