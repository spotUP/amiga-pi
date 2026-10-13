/* see agent.h */
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "context.h"
#include "image.h"
#include "ext_events.h"
#include "templates.h"
#include "llm/transform.h"
#include "llm/handoff.h"
#include "llm/estimate.h"
#include "resources.h"        /* s->res->skills: /skill:name */
#include "claude/util.h"
#include "claude/repl.h"        /* cl_key_clean */

long pi_agent_rounds;
long pi_agent_prompts;

static const char no_args[] = "{}";

void pi_session_init(pi_session *s, const llm_model *m, const char *key, llm_pool *pool, const llm_hooks *h)
{
    memset(s, 0, sizeof(*s));
    s->model = *m;
    s->key = key;
    s->pool = pool;
    if (h)
        s->h = *h;
    s->user_agent = "UP-Term-amiga-pi/0.1 (AmigaOS)";
    s->thinking = LT_OFF;
    s->retry_on = 1;            /* pi's retry settings: enabled, 3 retries, the first wait 2 s */
    s->retry_max = 3;
    s->retry_base = 2000;
    s->retry_max_delay = 60000;
    jw_init(&s->structured);
    jw_init(&s->details);
    jw_init(&s->usage);
    pi_emitter_init(&s->em);
    pi_exts_init(&s->ext);
    pi_subscribe(&s->em, pi_exts_event, &s->ext);
    pi_ev_attach(s);
}

void pi_session_clear(pi_session *s)
{
    int i;
    for (i = 0; i < s->nmsg; i++) {
        free(s->msg[i]);
        free(s->alt[i]);
        s->alt[i] = 0;
    }
    s->nmsg = 0;
    for (i = 0; i < s->nsys; i++)
        free(s->sys[i]);
    s->nsys = 0;
    s->sys_seen = 0;
}

int pi_session_push_system(pi_session *s, const char *json, long n)
{
    char *c;
    if (s->nsys == s->syscap) {
        int nc = s->syscap ? s->syscap * 2 : 8;
        char **gs = (char **)malloc((size_t)nc * sizeof(*gs));
        long *gn = (long *)malloc((size_t)nc * sizeof(*gn));
        int *ga = (int *)malloc((size_t)nc * sizeof(*ga));
        if (!gs || !gn || !ga) {
            free(gs);
            free(gn);
            free(ga);
            s->oom = 1;
            return -1;
        }
        if (s->nsys) {
            memcpy(gs, s->sys, (size_t)s->nsys * sizeof(*gs));
            memcpy(gn, s->sys_n, (size_t)s->nsys * sizeof(*gn));
            memcpy(ga, s->sys_at, (size_t)s->nsys * sizeof(*ga));
        }
        free(s->sys);
        free(s->sys_n);
        free(s->sys_at);
        s->sys = gs;
        s->sys_n = gn;
        s->sys_at = ga;
        s->syscap = nc;
    }
    c = (char *)malloc((size_t)n + 1);
    if (!c) {
        s->oom = 1;
        return -1;
    }
    memcpy(c, json, (size_t)n);
    c[n] = 0;
    s->sys[s->nsys] = c;
    s->sys_n[s->nsys] = n;
    s->sys_at[s->nsys] = s->nmsg;
    s->nsys++;
    return 0;
}

void pi_session_free(pi_session *s)
{
    int i;
    pi_session_clear(s);
    for (i = 0; i < s->nsteer; i++)
        free(s->steer[i]);
    for (i = 0; i < s->nfollow; i++)
        free(s->follow[i]);
    s->nsteer = s->nfollow = 0;
    for (i = 0; i < s->npend; i++)
        free(s->pend[i]);
    s->npend = 0;
    jw_free(&s->structured);
    jw_free(&s->details);
    jw_free(&s->usage);
    free(s->sys);
    free(s->sys_n);
    free(s->sys_at);
    s->sys = 0;
    s->sys_n = 0;
    s->sys_at = 0;
    s->syscap = 0;
    free(s->msg);
    free(s->msg_n);
    free(s->alt);
    free(s->alt_n);
    s->msg = 0;
    s->msg_n = 0;
    s->alt = 0;
    s->alt_n = 0;
    s->cap = 0;
}

int pi_tool_add(pi_session *s, pi_tool *t)
{
    if (s->ntools == PI_TOOLS)
        return -1;
    s->tools[s->ntools++] = t;
    return 0;
}

static llm_ts now(pi_session *s)
{
    llm_ts t;
    if (s->now)
        return s->now(s->h.u);
    t.sec = 0;
    t.ms = 0;
    return t;
}

/* a point on the millisecond clock (pi's performance.now()) and the wall time with it: the clock when the platform has
 * one, else the wall time stands in */
typedef struct tick {
    unsigned long ms;
    llm_ts ts;
} tick;

static tick tick_now(pi_session *s)
{
    tick t;
    t.ms = s->h.ms ? s->h.ms(s->h.u) : 0;
    t.ts = now(s);
    return t;
}

/* ms since t, rounded and never below 0 (pi: Math.max(0, Math.round(performance.now() - startedAt))) */
static long tick_since(pi_session *s, const tick *t)
{
    tick e = tick_now(s);
    long d = s->h.ms ? (long)(e.ms - t->ms) : (e.ts.sec - t->ts.sec) * 1000L + (e.ts.ms - t->ts.ms);
    return d < 0 ? 0 : d;
}

/* the message in w appended to the conversation (w keeps nothing): its index, -1 */
static int keep(pi_session *s, jw *w)
{
    if (w->oom || !w->p) {
        s->oom = 1;
        return -1;
    }
    if (s->nmsg == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        char **gm = (char **)malloc((size_t)nc * sizeof(*gm));
        long *gn = (long *)malloc((size_t)nc * sizeof(*gn));
        char **ga = (char **)calloc((size_t)nc, sizeof(*ga));
        long *gan = (long *)calloc((size_t)nc, sizeof(*gan));
        if (!gm || !gn || !ga || !gan) {
            free(gm);
            free(gn);
            free(ga);
            free(gan);
            s->oom = 1;
            return -1;
        }
        if (s->nmsg) {
            memcpy(gm, s->msg, (size_t)s->nmsg * sizeof(*gm));
            memcpy(gn, s->msg_n, (size_t)s->nmsg * sizeof(*gn));
            memcpy(ga, s->alt, (size_t)s->nmsg * sizeof(*ga));
            memcpy(gan, s->alt_n, (size_t)s->nmsg * sizeof(*gan));
        }
        free(s->msg);
        free(s->msg_n);
        free(s->alt);
        free(s->alt_n);
        s->msg = gm;
        s->msg_n = gn;
        s->alt = ga;
        s->alt_n = gan;
        s->cap = nc;
    }
    {
        /* an answer's usage: the context in use and the cost so far */
        jv m, v;
        if (!json_parse(w->p, w->n, &m) && json_get(m, "role", &v) && json_streq(v, "assistant") &&
            json_get(m, "usage", &v)) {
            llm_usage u;
            llm_r_usage(v, &u);
            if (u.input + u.output + u.cache_read + u.cache_write > 0)
                s->ctx_tokens = u.input + u.output + u.cache_read + u.cache_write;
            s->cost_micro += u.cost[0] + u.cost[1] + u.cost[2] + u.cost[3];
            s->tok_in += u.input;
            s->tok_out += u.output;
            s->tok_cr += u.cache_read;
            s->tok_cw += u.cache_write;
        }
    }
    s->msg[s->nmsg] = w->p;
    s->msg_n[s->nmsg] = w->n;
    s->alt[s->nmsg] = 0;
    s->alt_n[s->nmsg] = 0;
    w->p = 0;
    w->n = w->cap = 0;
    return s->nmsg++;
}

int pi_session_alt(pi_session *s, int i, const char *json, long n)
{
    char *c = 0;
    if (i < 0 || i >= s->nmsg)
        return -1;
    if (json) {
        c = (char *)malloc((size_t)n + 1);
        if (!c)
            return -1;
        memcpy(c, json, (size_t)n);
        c[n] = 0;
    }
    free(s->alt[i]);
    s->alt[i] = c;
    s->alt_n[i] = json ? n : 0;
    return 0;
}

int pi_session_push(pi_session *s, const char *json, long n)
{
    jw w;
    jw_init(&w);
    jw_raw(&w, json, n);
    return keep(s, &w) < 0 ? -1 : 0;
}

/* message_end goes to the extensions through emitMessageEnd, which lets a handler replace the message (same role) before the
 * listeners see it and the session saves it; every message end passes here (idx the conversation's index, -1 a message that is
 * not in it: a system message, a custom message made elsewhere) */
static void end_message(pi_session *s, pi_event *e, int idx)
{
    jw r;
    jw_init(&r);
    if (pi_ev_message_end(s, e->msg, e->msg_n, &r) && idx >= 0 && !r.oom) {
        free(s->msg[idx]);
        s->msg[idx] = r.p;
        s->msg_n[idx] = r.n;
        r.p = 0;
        r.n = r.cap = 0;
        e->msg = s->msg[idx];
        e->msg_n = s->msg_n[idx];
    }
    jw_free(&r);
    pi_emit(&s->em, e);
}

static void emit_msg(pi_session *s, int type, int i, int role)
{
    pi_event e;
    pi_event_init(&e, type);
    e.role = role;
    if (i >= 0) {
        e.msg = s->msg[i];
        e.msg_n = s->msg_n[i];
    }
    if (type == PE_MESSAGE_END && i >= 0)
        end_message(s, &e, i);
    else
        pi_emit(&s->em, &e);
}

static void emit_simple(pi_session *s, int type)
{
    pi_event e;
    pi_event_init(&e, type);
    e.stop = s->stop;
    pi_emit(&s->em, &e);
}

/* the transcript's system messages made since the last time as message events (pi's declareToolChanges emits them
 * through the agent: after turn_start, before the pending messages) */
static void announce_system(pi_session *s)
{
    if (s->sys_seen > s->nsys)
        s->sys_seen = s->nsys;
    while (s->sys_seen < s->nsys) {
        int j = s->sys_seen++, t;
        for (t = PE_MESSAGE_START; t <= PE_MESSAGE_END; t += PE_MESSAGE_END - PE_MESSAGE_START) {
            pi_event e;
            pi_event_init(&e, t);
            e.role = LR_SYSTEM;
            e.msg = s->sys[j];
            e.msg_n = s->sys_n[j];
            if (t == PE_MESSAGE_END)
                end_message(s, &e, -1);
            else
                pi_emit(&s->em, &e);
        }
    }
}

static void emit_queue(pi_session *s)
{
    pi_event e;
    pi_event_init(&e, PE_QUEUE_UPDATE);
    pi_emit(&s->em, &e);
}

int pi_queue_len(const pi_session *s, int follow)
{
    return (follow ? s->nfollow : s->nsteer) + (s->npend && s->pend_follow == follow ? s->npend : 0);
}

void pi_queue_json(const pi_session *s, int follow, jw *w)
{
    int i, k = 0;
    jw_rawz(w, "[");
    if (s->npend && s->pend_follow == follow)
        for (i = 0; i < s->npend; i++) {
            if (k++)
                jw_rawz(w, ",");
            jw_strz(w, s->pend[i]);
        }
    {
        char *const *q = follow ? s->follow : s->steer;
        int n = follow ? s->nfollow : s->nsteer;
        for (i = 0; i < n; i++) {
            if (k++)
                jw_rawz(w, ",");
            jw_strz(w, q[i]);
        }
    }
    jw_rawz(w, "]");
}

/* the messages from message index m0 and system message index sys0 on (the transcript's system messages in their places) as
 * a JSON array: agent_end's `messages` for the run, RPC's get_messages for the whole conversation */
void pi_messages_json(const pi_session *s, int m0, int sys0, jw *w)
{
    int k, j = sys0 < s->nsys ? sys0 : s->nsys, first = 1;
    if (m0 > s->nmsg)
        m0 = s->nmsg;
    jw_rawz(w, "[");
    for (k = m0; k <= s->nmsg; k++) {
        while (j < s->nsys && s->sys_at[j] <= k) {
            if (!first)
                jw_rawz(w, ",");
            first = 0;
            jw_raw(w, s->sys[j], s->sys_n[j]);
            j++;
        }
        if (k < s->nmsg) {
            if (!first)
                jw_rawz(w, ",");
            first = 0;
            if (s->alt[k])
                jw_raw(w, s->alt[k], s->alt_n[k]);
            else
                jw_raw(w, s->msg[k], s->msg_n[k]);
        }
    }
    jw_rawz(w, "]");
}

/* turn_end: the assistant message i and the tool results after it; 1 when an extension asked the run to go on */
static int emit_turn_end(pi_session *s, int i)
{
    int go = 0;
    pi_event e;
    jw w;
    int k, first = 1;
    jw_init(&w);
    jw_rawz(&w, "[");
    for (k = i + 1; k < s->nmsg; k++) {
        llm_info inf;
        if (llm_msg_info(s->msg[k], s->msg_n[k], &inf) < 0 || inf.role != LR_TOOLRESULT)
            continue;
        if (!first)
            jw_rawz(&w, ",");
        first = 0;
        jw_raw(&w, s->msg[k], s->msg_n[k]);
    }
    jw_rawz(&w, "]");
    pi_event_init(&e, PE_TURN_END);
    e.stop = s->stop;
    if (i >= 0 && i < s->nmsg) {
        e.msg = s->msg[i];
        e.msg_n = s->msg_n[i];
        e.role = LR_ASSISTANT;
    }
    e.result = w.p;
    e.result_n = w.n;
    if (i >= 0 && i < s->nmsg)
        go = pi_ev_turn_end(s, s->msg[i], s->msg_n[i], w.p, w.n);       /* finishTurn: before the agent's own turn_end */
    pi_emit(&s->em, &e);
    jw_free(&w);
    return go;
}

/* ---- one answer ---- */

typedef struct stream {
    pi_session *s;
    llm_acc acc;
    int started;
} stream;

static void st_ev(void *u, const llm_event *e)
{
    stream *t = (stream *)u;
    pi_event pe;
    llm_acc_event(&t->acc, e);
    if (e->type == LE_START && !t->started) {
        t->started = 1;
        pi_event_init(&pe, PE_MESSAGE_START);
        pe.role = LR_ASSISTANT;
        pe.acc = &t->acc;
        pi_emit(&t->s->em, &pe);
    } else if (t->started && (e->type == LE_BLOCK_START || e->type == LE_DELTA || e->type == LE_SIG_DELTA ||
                              e->type == LE_BLOCK_END)) {
        pi_event_init(&pe, PE_MESSAGE_UPDATE);
        pe.role = LR_ASSISTANT;
        pe.ae = e;
        pe.acc = &t->acc;
        pi_emit(&t->s->em, &pe);
    }
}

/* the tools as pi-ai's request carries them */
void pi_tools_json(const pi_session *s, jw *w)
{
    int i;
    jw_rawz(w, "[");
    for (i = 0; i < s->ntools; i++) {
        const pi_tool *t = s->tools[i];
        jw_rawz(w, i ? ",{\"name\":" : "{\"name\":");
        jw_strz(w, t->name);
        jw_rawz(w, ",\"description\":");
        jw_strz(w, t->description ? t->description : "");
        jw_rawz(w, ",\"parameters\":");
        jw_rawz(w, t->parameters ? t->parameters : "{\"type\":\"object\",\"properties\":{}}");
        if (t->constrained) {
            jw_rawz(w, ",\"constrainedSampling\":");
            jw_rawz(w, t->constrained);
        }
        jw_rawz(w, "}");
    }
    jw_rawz(w, "]");
}

int pi_req_auth(pi_session *s, llm_req *q, jw *hdr)
{
    q->key = s->key;
    q->xhead = 0;
    llm_set_wall(s->now, s->h.u);       /* google's generated tool-call ids read the wall clock (pi: Date.now()) */
    if (!s->cfg)
        return 0;
    if (llm_cfg_endpoint(s->cfg, &s->model, s->err, sizeof(s->err)))
        return -1;                      /* azure-openai-responses: AZURE_OPENAI_* (pi's resolveAzureConfig) */
    s->keybuf[0] = 0;
    llm_cfg_key(s->cfg, &s->model, s->keybuf, sizeof(s->keybuf), 0);
    if (s->keybuf[0] && cl_key_clean(s->keybuf))
        s->keybuf[0] = 0;           /* not a key (C:Claude's rule) */
    q->key = s->keybuf[0] ? s->keybuf : 0;
    if (llm_cfg_headers(s->cfg, &s->model, q->key, hdr)) {
        cl_copy(s->err, "a header value of models.json for ", sizeof(s->err));
        cl_cat(s->err, s->model.provider, sizeof(s->err));
        cl_cat(s->err, " could not be resolved", sizeof(s->err));
        return -1;
    }
    q->xhead = hdr->n ? hdr->p : 0;
    return 0;
}

/* the request and the answer kept: the answer's index, -1 out of memory */
static int answer(pi_session *s, stream *t)
{
    llm_view v;
    llm_ctx c;
    llm_req q;
    llm_sink k;
    jw tools, m, hdr;
    int i, rc;
    const char *const *pm = (const char *const *)s->msg;
    const long *pn = s->msg_n;
    int pc = s->nmsg;
    const char **tm = 0;
    long *tn = 0;
    long est;
    int o = 0;
    tick t0;
    jw cj;                      /* the messages a context handler rewrote: tm points into it */
    const char **xm = 0;        /* the same without the system messages, for a model that takes none in the conversation */
    long *xn = 0;
    char **own = 0;             /* messages converted from AgentMessages a handler left in */
    int nown = 0, k2;
    jw_init(&cj);
    {
        /* pi's agent-loop declareToolChanges runs before every request, whatever the model: the tool changes become a
         * system message of the transcript (the session file); a model that takes no system messages in the
         * conversation gets the collapsed prompt and tool list in the request, the same either way */
        int k, j = 0;
        if (s->declare)
            s->declare(s->declare_u, s);
        announce_system(s);
        /* the transcript's system messages in place (the backend reads the first as the initial one) */
        tm = (const char **)malloc((size_t)(s->nmsg + s->nsys + 1) * sizeof(*tm));
        tn = (long *)malloc((size_t)(s->nmsg + s->nsys + 1) * sizeof(*tn));
        if (!tm || !tn) {
            free(tm);
            free(tn);
            s->oom = 1;
            return -1;
        }
        for (k = 0; k <= s->nmsg; k++) {
            while (j < s->nsys && s->sys_at[j] <= k) {
                tm[o] = s->sys[j];
                tn[o++] = s->sys_n[j++];
            }
            if (k < s->nmsg) {
                tm[o] = s->msg[k];
                tn[o++] = s->msg_n[k];
            }
        }
    }
    /* the extensions' context and context_with_system events (runner.ts emitContext): the request carries what they left */
    if (pi_ev_context(s, &cj)) {
        jv arr, el, r;
        jit it;
        long cnt;
        const char **nm;
        long *nn;
        if (!json_parse(cj.p, cj.n, &arr) && json_type(arr) == J_ARR) {
            cnt = json_count(arr);
            nm = (const char **)malloc((size_t)(cnt + 1) * sizeof(*nm));
            nn = (long *)malloc((size_t)(cnt + 1) * sizeof(*nn));
            xm = (const char **)malloc((size_t)(cnt + 1) * sizeof(*xm));
            xn = (long *)malloc((size_t)(cnt + 1) * sizeof(*xn));
            own = (char **)calloc((size_t)(cnt + 1), sizeof(*own));
            if (nm && nn && xm && xn && own) {
                int nx = 0;
                free(tm);
                free(tn);
                tm = nm;
                tn = nn;
                o = 0;
                json_iter(arr, &it);
                while (json_next(&it, 0, &el)) {
                    int sys = json_get(el, "role", &r) && json_streq(r, "system");
                    const char *mp = el.p;
                    long mn = el.n;
                    if (!sys && !(json_get(el, "role", &r) && (json_streq(r, "user") || json_streq(r, "assistant") ||
                                                              json_streq(r, "toolResult")))) {
                        /* an AgentMessage (custom, bashExecution, ...) a handler left in: the request carries convertToLlm's */
                        jw one;
                        jw_init(&one);
                        if (ctx_msg_to_llm(el.p, el.n, &one) && !one.oom && one.p && nown < cnt) {
                            own[nown] = one.p;
                            mp = one.p;
                            mn = one.n;
                            nown++;
                        } else {
                            jw_free(&one);
                            continue;       /* convertToLlm leaves it out */
                        }
                    }
                    tm[o] = mp;
                    tn[o++] = mn;
                    if (!sys) {
                        xm[nx] = mp;
                        xn[nx++] = mn;
                    }
                }
                pm = xm;
                pn = xn;
                pc = nx;
            } else {
                free(nm);
                free(nn);
                free(xm);
                free(xn);
                free(own);
                own = 0;
                xm = 0;
                xn = 0;
            }
        }
    }
    /* pi's buildBaseOptions: the output cap is clamped by the estimate over this transcript, as it stands */
    est = llm_estimate_context(tm, tn, o);
    if (llm_mid_system(&s->model)) {
        pm = tm;
        pn = tn;
        pc = o;
    }
    t0 = tick_now(s);           /* the request's start: the message's timestamp, and durationMs counts from here */
    rc = llm_prepare(&s->model, pm, pn, pc, t0.ts, &v);
    free(tm);
    free(tn);
    free(xm);
    free(xn);
    if (rc) {
        llm_view_free(&v);
        jw_free(&cj);
        for (k2 = 0; k2 < nown; k2++)
            free(own[k2]);
        free(own);
        s->oom = 1;
        return -1;
    }
    jw_init(&tools);
    if (s->ntools)
        pi_tools_json(s, &tools);
    memset(&c, 0, sizeof(c));
    c.model = &s->model;
    c.system = s->system;
    c.session_id = s->session_id;
    c.est_tokens = est;
    c.msgs = v.m;
    c.msg_n = v.n;
    c.nmsg = v.count;
    c.tools = s->ntools ? tools.p : 0;
    c.tools_n = tools.n;
    c.thinking = s->thinking;
    llm_acc_init(&t->acc, s->model.api, s->model.provider);
    llm_acc_model(&t->acc, s->model.id, s->model.cost);
    llm_acc_fallbacks(&t->acc, s->model.compat.fb, s->model.compat.nfb);
    llm_acc_level(&t->acc, llm_ant_effort(&s->model, s->thinking));
    llm_acc_thinking(&t->acc, llm_think_name(s->thinking));     /* pi-agent-core: the level the loop asked at, "off" too */
    k.u = t;
    k.ev = st_ev;
    memset(&q, 0, sizeof(q));
    q.pool = s->pool;
    q.retries = -1;             /* pi retries a failed answer itself (agent-session.js), with its events */
    q.model = &s->model;
    q.be = llm_backend_of(s->model.api);
    q.ctx = &c;
    q.user_agent = s->user_agent;
    q.h = s->h;
    q.sink = &k;
    jw_init(&hdr);
    if (pi_req_auth(s, &q, &hdr)) {
        llm_event e;
        char why[256];
        cl_copy(why, s->err, sizeof(why));
        llm_event_init(&e, LE_ERROR);
        e.stop = LS_ERROR;
        e.err_type = "error";
        e.p = why;
        e.n = (long)strlen(why);
        st_ev(t, &e);
    } else if (!q.be) {
        llm_event e;
        llm_event_init(&e, LE_ERROR);
        e.stop = LS_ERROR;
        e.err_type = "error";
        e.p = "No API provider registered for this model's api";
        e.n = (long)strlen(e.p);
        st_ev(t, &e);
    } else
        llm_request(&q);
    llm_view_free(&v);
    jw_free(&cj);
    for (k2 = 0; k2 < nown; k2++)
        free(own[k2]);
    free(own);
    jw_free(&tools);
    jw_free(&hdr);
    jw_init(&m);
    /* pi-ai 1.1.0 (event-stream.ts #time): durationMs is set when the stream's done/error event is pushed */
    llm_acc_duration(&t->acc, tick_since(s, &t0));
    llm_acc_message(&t->acc, t0.ts, &m);
    i = keep(s, &m);
    jw_free(&m);
    if (i >= 0) {
        llm_info inf;
        if (llm_msg_info(s->msg[i], s->msg_n[i], &inf) >= 0 && inf.stop != LS_ERROR && inf.stop != LS_LENGTH)
            s->overflow_tried = 0;      /* pi: message_end of an answer that is neither an error nor a length stop */
    }
    return i;
}

/* ---- tool calls ---- */

static pi_tool *tool_of(pi_session *s, const char *name)
{
    int i;
    for (i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i]->name, name))
            return s->tools[i];
    return 0;
}

static void text_result(jw *content, const char *text)
{
    jw_reset(content);
    jw_rawz(content, "[");
    llm_b_text(content, text, (long)strlen(text), 0);
    jw_rawz(content, "]");
}

/* one call through the extensions and the tool, its toolResult kept; 1 when the user stopped it */
/* *terminate: this call finalized with terminate (agent-loop.ts shouldTerminateToolBatch asks every call of the batch) */
static int run_call(pi_session *s, const llm_ablock *b, int length_stop, int *terminate)
{
    pi_event e;
    jw args, content, m;
    int is_error = 0, stopped = 0, i, thrown = -1, early_usage = 0;
    long duration = -1;         /* pi-agent-core 1.1.0: the tool's execute() time; none when it did not run */
    char reason[256], what[160];
    pi_tool *t;
    jw_init(&args);
    jw_init(&content);
    jw_reset(&s->structured);
    jw_reset(&s->details);
    jw_reset(&s->usage);
    s->call_terminate = 0;
    *terminate = 0;
    s->result_error = 0;
    if (b->a.n)
        jw_raw(&args, b->a.p, b->a.n);
    else
        jw_rawz(&args, no_args);
    pi_event_init(&e, PE_TOOL_START);
    e.tool_id = b->id;
    e.tool_name = b->name;
    e.args = args.p;
    e.args_n = args.n;
    pi_emit(&s->em, &e);
    t = tool_of(s, b->name);
    if (length_stop) {
        /* pi: failToolCallsFromTruncatedMessage (agent-loop.ts:478-506) */
        jw tx;
        jw_init(&tx);
        jw_rawz(&tx, "Tool call \"");
        jw_rawz(&tx, b->name);
        jw_rawz(&tx, "\" was not executed: the response hit the output token limit, so its arguments may be "
                     "truncated. Re-issue the tool call with complete arguments.");
        text_result(&content, tx.p ? tx.p : "");
        jw_free(&tx);
        is_error = 1;
    } else if (!t) {
        cl_copy(what, "Tool ", sizeof(what));
        cl_cat(what, b->name, sizeof(what));
        cl_cat(what, " not found", sizeof(what));
        text_result(&content, what);
        is_error = 1;
    } else if (pi_exts_tool_call(&s->ext, b->id, b->name, &args, reason, sizeof(reason))) {
        text_result(&content, reason[0] ? reason : "Tool execution was blocked");
        is_error = 1;
        *terminate = s->ext.terminate;
    } else if (s->h.poll && s->h.poll(s->h.u)) {
        text_result(&content, "Operation aborted");
        is_error = 1;
        stopped = 1;
    } else {
        t->calls++;
        s->call_id = b->id;
        s->call_args = b->a.n ? b->a.p : no_args;       /* what the model sent: the tool's events carry it, whatever an extension rewrote */
        s->call_args_n = b->a.n ? b->a.n : (long)strlen(no_args);
        {
            tick t1 = tick_now(s);
            stopped = t->run(t->u, s, args.p, args.n, &content, &is_error);
            duration = tick_since(s, &t1);      /* around the tool only: not the extensions' tool_result hook below */
        }
        s->call_id = 0;
        s->call_args = 0;
        if (!content.n)
            jw_rawz(&content, "[]");
        thrown = is_error && !s->result_error;  /* the tool's own failure: its result has details {}; an extension's isError does not */
        s->ext.structured = &s->structured;     /* a tool_result extension may replace them with the content */
        s->ext.details = &s->details;
        s->ext.usage = &s->usage;
        s->ext.thrown = thrown;
        *terminate = s->call_terminate;
        early_usage = s->usage.n != 0;
        pi_exts_tool_result(&s->ext, b->id, b->name, args.p, args.n, &content, &is_error);
        s->ext.structured = s->ext.details = 0;
        s->ext.usage = 0;
    }
    pi_event_init(&e, PE_TOOL_END);
    e.tool_id = b->id;
    e.tool_name = b->name;
    e.args = args.p;
    e.args_n = args.n;
    e.result = content.p;
    e.result_n = content.n;
    e.is_error = is_error;
    e.result_error = s->result_error;
    if (thrown < 0)
        thrown = is_error;
    e.thrown = thrown;
    e.structured = s->structured.n ? s->structured.p : 0;
    e.structured_n = s->structured.n;
    e.details = s->details.n ? s->details.p : 0;
    e.details_n = s->details.n;
    e.duration_ms = duration;
    e.usage = s->usage.n ? s->usage.p : 0;
    e.usage_n = s->usage.n;
    e.terminate = *terminate;
    e.usage_early = early_usage;
    pi_emit(&s->em, &e);
    jw_init(&m);
    llm_m_toolresult(&m, b->id, b->name, content.p, content.n,
                     s->details.n ? s->details.p : thrown && !s->structured.n ? "{}" : 0,
                     s->details.n ? s->details.n : thrown && !s->structured.n ? 2 : 0, is_error, duration, now(s));
    if (s->usage.n && m.p && !m.oom) {
        /* the message's usage member sits between details and isError (agent-loop.ts createToolResultMessage); llm_m_toolresult
         * (llm/, shared with C:Claude) has no parameter for it */
        char *at = strstr(m.p, ",\"isError\":");
        if (at) {
            jw u;
            long off = (long)(at - m.p);
            jw_init(&u);
            jw_raw(&u, m.p, off);
            jw_rawz(&u, ",\"usage\":");
            jw_raw(&u, s->usage.p, s->usage.n);
            jw_raw(&u, m.p + off, m.n - off);
            jw_free(&m);
            m = u;
        }
    }
    i = keep(s, &m);
    jw_free(&m);
    if (i >= 0) {
        emit_msg(s, PE_MESSAGE_START, i, LR_TOOLRESULT);
        emit_msg(s, PE_MESSAGE_END, i, LR_TOOLRESULT);
    }
    jw_free(&args);
    jw_free(&content);
    return stopped;
}

/* ---- the loop ---- */

/* ---- pi's isRetryableAssistantError (pi-ai utils/retry.js) and isContextOverflow (utils/overflow.js): the error message
 * against lists of patterns, case insensitive substrings; "." is any character, "x?" makes x optional, "*" any run of
 * characters, "~" any white space and "#" a number ([\d,]+), as in the regular expressions there ---- */

static const char *const retry_pat[] = {
    "overloaded", "server_busy", "servers are currently busy", "currently experiencing high demand",
    "model is at capacity", "rate.?limit", "too many requests", "429", "500", "502", "503", "504", "520", "524",
    "service.?unavailable", "server.?error", "internal.?error", "provider.?returned.?error",
    "exceeded request buffer limit while retrying upstream", "network.?error", "connection.?error",
    "connection.?refused", "connection.?lost", "other side closed", "fetch failed", "getaddrinfo", "ENOTFOUND",
    "EAI_AGAIN", "upstream.?connect", "reset before headers", "socket hang up", "socket connection was closed",
    "timed? out", "timeout", "terminated", "websocket.?closed", "websocket.?error", "ended without",
    "stream ended before message_stop", "stream ended before a terminal response event",
    "http2 request did not get a response", "pending stream has been canceled", "retry delay",
    "you can retry your request", "try your request again", "please retry your request", "ResourceExhausted",
    "subscription_sharing_usage_unavailable", "subscription_sharing_user_unavailable", 0
};

static const char *const norm_pat[] = {
    "GoUsageLimitError", "FreeUsageLimitError", "Monthly usage limit reached", "available balance",
    "insufficient_quota", "out of budget", "quota exceeded", "billing", "subscription_sharing_usage_limit_exceeded", 0
};

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* pattern p at the start of t */
static int pat_at(const char *t, const char *p)
{
    int k;
    if (!*p)
        return 1;
    if (p[0] == '*') {
        for (k = 0;; k++) {
            if (pat_at(t + k, p + 1))
                return 1;
            if (!t[k])
                return 0;
        }
    }
    if (p[0] == '~') {
        for (k = 0;; k++) {
            if (pat_at(t + k, p + 1))
                return 1;
            if (t[k] != ' ' && t[k] != '\t' && t[k] != '\n' && t[k] != '\r')
                return 0;
        }
    }
    if (p[0] == '#') {
        for (k = 1; (t[k - 1] >= '0' && t[k - 1] <= '9') || t[k - 1] == ','; k++)
            if (pat_at(t + k, p + 1))
                return 1;
        return 0;
    }
    if (p[1] == '?') {
        if (pat_at(t, p + 2))
            return 1;
        return *t && (p[0] == '.' || lower(*t) == lower(p[0])) && pat_at(t + 1, p + 2);
    }
    return *t && (p[0] == '.' || lower(*t) == lower(p[0])) && pat_at(t + 1, p + 1);
}

static int pat_any(const char *t, const char *const *list)
{
    int i;
    for (i = 0; list[i]; i++) {
        const char *x;
        for (x = t; ; x++) {
            if (pat_at(x, list[i]))
                return 1;
            if (!*x)
                break;
        }
    }
    return 0;
}

/* pi-ai's OVERFLOW_PATTERNS, each (?:x )? written out as its two spellings */
static const char *const overflow_pat[] = {
    "prompt too long", "prompt is too long", "prompt exceeds max length", "request_too_large",
    "input is too long for requested model", "exceeds the context window",
    "exceeds maximum context length of # token", "exceeds the maximum context length of # token",
    "exceeds model'?s maximum context length of # token", "exceeds the model'?s maximum context length of # token",
    "exceeds maximum context length~(#)", "exceeds the maximum context length~(#)",
    "exceeds model'?s maximum context length~(#)", "exceeds the model'?s maximum context length~(#)",
    "input token count*exceeds the maximum", "maximum prompt length is #", "reduce the length of the messages",
    "maximum context length is # tokens", "exceeds maximum allowed input length of # token",
    "exceeds the maximum allowed input length of # token", "input (# tokens) is longer than the model'?s context length (# tokens)",
    "exceeds the limit of #", "exceeds the available context size", "greater than the context length",
    "context window exceeds limit", "exceeded model token limit", "too large for model with # maximum context length",
    "prompt has # tokens?, but the configured context size is # tokens?", "model_context_window_exceeded",
    "prompt too long; exceeded context length", "prompt too long; exceeded max context length",
    "range of input length should be", "context_length_exceeded", "context_length exceeded", "context length_exceeded",
    "context length exceeded", "too many tokens", "token limit exceeded", 0
};

/* NON_OVERFLOW_PATTERNS: throttling and rate limits that read like an overflow ("Too many tokens, please wait") */
static const char *const not_overflow_pat[] = { "rate limit", "too many requests", 0 };

int pi_context_overflow(const char *em)
{
    if (!em || !*em)
        return 0;
    if (pat_at(em, "Throttling error:") || pat_at(em, "Service unavailable:") || pat_any(em, not_overflow_pat))
        return 0;
    return pat_any(em, overflow_pat);
}

int pi_retryable(const char *em)
{
    if (!em || !*em)
        return 0;
    return !pat_any(em, norm_pat) && pat_any(em, retry_pat);
}

/* the run just ended on a failure pi retries (agent-session.js _willRetryAfterAgentEnd, _isRetryableError: a context overflow
 * is not retried, the compaction check after the run takes it) */
static int retry_will(const pi_session *s)
{
    return s->retry_on && s->attempt < s->retry_max && s->stop == LS_ERROR && !s->retry_stop && !pi_context_overflow(s->err) &&
           pi_retryable(s->err);
}

static void emit_retry_end(pi_session *s, int success, const char *final_error)
{
    pi_event e;
    pi_event_init(&e, PE_RETRY_END);
    e.retry = success;
    e.n1 = s->attempt;
    e.msg = final_error;
    e.msg_n = final_error ? (long)strlen(final_error) : 0;
    pi_emit(&s->em, &e);
    s->attempt = 0;
}

/* agent_end, then pi's check after the run when it gave an answer */
static int run_end(pi_session *s, int answered)
{
    pi_event e;
    jw w;
    int will = answered && retry_will(s);
    jw_init(&w);
    pi_messages_json(s, s->run_msg0, s->run_sys0, &w);
    pi_event_init(&e, PE_AGENT_END);
    e.stop = s->stop;
    e.result = w.p;
    e.result_n = w.n;
    e.retry = will;
    pi_emit(&s->em, &e);
    jw_free(&w);
    s->retry_next = will;
    if (!will && s->attempt && s->stop == LS_ERROR)
        emit_retry_end(s, 0, s->err[0] ? s->err : "Unknown error");
    if (!will && answered && s->compact)
        s->recover = s->compact(s->compact_u, s, PI_CP_END) == 1;
    return s->stop;
}

/* agent-session.ts _normalizePromptImages: each image of the prompt (a JSON array of ImageContent) decoded and run through
 * processImage; the blocks that go into the message are appended to blocks (each led by a comma), the hints (or the reason an
 * image was left out) to hints, one per line */
static void normalize_images(const char *images, jw *blocks, jw *hints)
{
    jv arr, el, v;
    jit it;
    if (!images || json_parse(images, (long)strlen(images), &arr) || json_type(arr) != J_ARR)
        return;
    json_iter(arr, &it);
    while (json_next(&it, 0, &el)) {
        long dn, bn;
        char *data = json_get(el, "data", &v) && json_type(v) == J_STR ? json_strdup(v, &dn) : 0;
        unsigned char *bin = data ? pi_b64_decode(data, dn, &bn) : 0;
        char mt[40];
        const char *mime;
        pi_img im;
        mt[0] = 0;
        if (json_get(el, "mimeType", &v) && json_type(v) == J_STR)
            json_str(v, mt, sizeof(mt));
        free(data);
        if (!bin)
            continue;
        mime = pi_image_type(bin, bn);
        if (!mime)
            mime = mt[0] ? mt : "image/png";
        if (pi_image_process(bin, bn, mime, &im) == 0) {
            jw d;
            jw_init(&d);
            jw_base64(&d, im.data, im.n);
            jw_rawz(blocks, ",");
            llm_b_image(blocks, d.p ? d.p : "", im.mime);
            jw_free(&d);
        }
        if (im.note[0]) {
            if (hints->n)
                jw_rawz(hints, "\n");
            jw_rawz(hints, im.note);
        }
        pi_image_free(&im);
        free(bin);
    }
}

/* a user message (pi: a prompt's content is an array of blocks, the text first -- agent-session.js prompt) kept
 * and announced: its index, -1 out of memory. images: the prompt's images (a JSON array), 0 none; their hints follow the text */
static int add_user(pi_session *s, const char *text, const char *images)
{
    jw w, c, blocks, hints, full;
    int i;
    jw_init(&blocks);
    jw_init(&hints);
    jw_init(&full);
    normalize_images(images, &blocks, &hints);
    jw_rawz(&full, text);
    if (hints.n) {
        jw_rawz(&full, "\n\n");
        jw_raw(&full, hints.p, hints.n);
    }
    jw_init(&c);
    jw_rawz(&c, "[");
    llm_b_text(&c, full.p ? full.p : "", full.n, 0);
    if (blocks.n)
        jw_raw(&c, blocks.p, blocks.n);
    jw_rawz(&c, "]");
    jw_init(&w);
    llm_m_user(&w, c.p, c.n, now(s));
    jw_free(&c);
    jw_free(&blocks);
    jw_free(&hints);
    jw_free(&full);
    i = keep(s, &w);
    jw_free(&w);
    if (i >= 0) {
        s->overflow_tried = 0;          /* pi: message_start of a user message */
        emit_msg(s, PE_MESSAGE_START, i, LR_USER);
        emit_msg(s, PE_MESSAGE_END, i, LR_USER);
    }
    return i;
}

int pi_session_custom(pi_session *s, const char *custom_type, const char *content, int display, const char *details)
{
    jw m, one;
    pi_event e;
    int i, rc = -1;
    llm_ts t = now(s);
    jw_init(&m);
    jw_rawz(&m, "{\"role\":\"custom\",\"customType\":");
    jw_strz(&m, custom_type);
    jw_rawz(&m, ",\"content\":");
    jw_rawz(&m, content && content[0] ? content : "[]");
    jw_rawz(&m, display ? ",\"display\":true" : ",\"display\":false");
    if (details) {
        jw_rawz(&m, ",\"details\":");
        jw_rawz(&m, details);
    }
    jw_rawz(&m, ",\"timestamp\":");
    llm_w_ts(&m, t);
    jw_rawz(&m, "}");
    jw_init(&one);
    if (!m.oom && (!s->custom_save || s->custom_save(s->custom_u, s, custom_type, content && content[0] ? content : "[]", display, details) == 0) &&
        ctx_msg_to_llm(m.p, m.n, &one) && !one.oom) {
        i = keep(s, &one);
        if (i >= 0) {
            int ty;
            pi_session_alt(s, i, m.p, m.n);
            for (ty = PE_MESSAGE_START; ty <= PE_MESSAGE_END; ty += PE_MESSAGE_END - PE_MESSAGE_START) {
                pi_event_init(&e, ty);
                e.role = PI_ROLE_CUSTOM;
                e.msg = m.p;
                e.msg_n = m.n;
                if (ty == PE_MESSAGE_END)
                    end_message(s, &e, -1);
                else
                    pi_emit(&s->em, &e);
            }
            rc = 0;
        }
    }
    jw_free(&one);
    jw_free(&m);
    return rc;
}

/* a queued message is its text, a NUL, then the images of the message (a JSON array, "" none), a NUL */
static int queue_add(char **q, int *n, const char *text, const char *images)
{
    size_t tl = strlen(text), il = images ? strlen(images) : 0;
    char *c = (char *)malloc(tl + 1 + il + 1);
    if (!c || *n == PI_QUEUE) {
        free(c);
        return -1;
    }
    memcpy(c, text, tl + 1);
    if (il)
        memcpy(c + tl + 1, images, il);
    c[tl + 1 + il] = 0;
    q[(*n)++] = c;
    return 0;
}

/* pi's _expandSkillCommand, applied where pi_prompt, pi_steer and pi_follow_up take the text (agent-session.ts:2005-2010): the
 * text as typed, or the <skill> block in buf (buf is initialised here and freed by the caller) */
/* agent-session.ts prompt() (2005-2010) and _queueUserInput (2183-2192): a skill command expands, then a prompt template's
 * /name args; the one function for the prompt and for the queued messages. The text is returned as is when nothing expands. */
static const char *expand_input(const pi_session *s, const char *text, jw *buf)
{
    jw tm;
    jw_init(buf);
    if (s->res && s->res->skills.n && !strncmp(text, "/skill:", 7) && pi_skill_expand(&s->res->skills, text, buf) == 1)
        text = buf->p;
    if (s->templates && text[0] == '/') {
        jw_init(&tm);
        if (pi_tmpl_expand(s->templates, text, &tm) && !tm.oom && tm.p) {
            jw_reset(buf);
            jw_raw(buf, tm.p, tm.n);
            text = buf->p;
        }
        jw_free(&tm);
    }
    return text;
}

/* agent-session.ts _queueUserInput (2173-2199): the extensions' input event first (streamingBehavior "steer" or
 * "followUp"; "handled" queues nothing), then the skill command and the prompt template expand, then the message is queued */
static int queue_input(pi_session *s, const char *text, int follow, const char *images)
{
    jw t, ex, im;
    int rc;
    jw_init(&t);
    jw_init(&im);
    jw_rawz(&t, text);
    if (images && images[0])
        jw_rawz(&im, images);
    if (s->ext.n) {
        int handled;
        s->ext.streaming = follow ? "followUp" : "steer";
        s->ext.images = &im;
        handled = pi_exts_input(&s->ext, &t);
        s->ext.images = 0;
        s->ext.streaming = 0;
        if (handled) {
            jw_free(&t);
            jw_free(&im);
            return 0;
        }
    }
    rc = follow ? queue_add(s->follow, &s->nfollow, expand_input(s, t.p ? t.p : "", &ex), im.n ? im.p : 0)
                : queue_add(s->steer, &s->nsteer, expand_input(s, t.p ? t.p : "", &ex), im.n ? im.p : 0);
    jw_free(&ex);
    jw_free(&im);
    jw_free(&t);
    if (rc)
        return -1;
    emit_queue(s);
    return 0;
}

int pi_steer_images(pi_session *s, const char *text, const char *images)
{
    return queue_input(s, text, 0, images);
}

int pi_follow_up_images(pi_session *s, const char *text, const char *images)
{
    return queue_input(s, text, 1, images);
}

int pi_steer(pi_session *s, const char *text)
{
    return queue_input(s, text, 0, 0);
}

int pi_follow_up(pi_session *s, const char *text)
{
    return queue_input(s, text, 1, 0);
}

int pi_queue_take(pi_session *s, char **out, int max)
{
    int n = 0, i, had = s->nsteer + s->nfollow;
    for (i = 0; i < s->nsteer && n < max; i++)
        out[n++] = s->steer[i];
    s->nsteer = 0;
    for (i = 0; i < s->nfollow && n < max; i++)
        out[n++] = s->follow[i];
    s->nfollow = 0;
    if (had)
        emit_queue(s);
    return n;
}

/* agent-loop.js getSteeringMessages / getFollowUpMessages: the next queued message (all of them in "all" mode)
 * taken off the queue (polled: pi delivers them at the next turn); how many. They stay in s->pend, and in the queue the
 * session shows, until their user message is made. */
static int poll_queue(pi_session *s, int follow)
{
    char **q = follow ? s->follow : s->steer;
    int *n = follow ? &s->nfollow : &s->nsteer;
    int take = (follow ? s->follow_all : s->steer_all) ? *n : (*n ? 1 : 0), i;
    for (i = 0; i < take; i++)
        s->pend[i] = q[i];
    memmove(q, q + take, sizeof(q[0]) * (size_t)(*n - take));
    *n -= take;
    s->npend = take;
    s->pend_follow = follow;
    return take;
}

/* runLoop's pending messages before the next request: the tool changes declared first (declareToolChanges puts
 * its system message before the first pending one), then each as a user message */
static void deliver(pi_session *s)
{
    int n = s->npend;
    if (n && s->declare)
        s->declare(s->declare_u, s);
    announce_system(s);
    while (s->npend) {
        char *m = s->pend[0];
        memmove(s->pend, s->pend + 1, sizeof(s->pend[0]) * (size_t)(s->npend - 1));
        s->npend--;
        emit_queue(s);
        add_user(s, m, m + strlen(m) + 1);
        free(m);
    }
}

/* the custom messages before_agent_start's handlers made (pi pushes them after the user message) */
static void flush_next_custom(pi_session *s)
{
    int i;
    for (i = 0; i < s->nnext_custom; i++) {
        jv m, v;
        if (!json_parse(s->next_custom[i], (long)strlen(s->next_custom[i]), &m)) {
            char ct[200];
            jw content, details;
            ct[0] = 0;
            jw_init(&content);
            jw_init(&details);
            if (json_get(m, "customType", &v) && json_type(v) == J_STR)
                json_str(v, ct, sizeof(ct));
            if (json_get(m, "content", &v))
                jw_raw(&content, v.p, v.n);
            if (json_get(m, "details", &v))
                jw_raw(&details, v.p, v.n);
            pi_session_custom(s, ct, content.p, json_get(m, "display", &v) && json_type(v) == J_TRUE, details.n ? details.p : 0);
            jw_free(&content);
            jw_free(&details);
        }
        free(s->next_custom[i]);
        s->next_custom[i] = 0;
    }
    s->nnext_custom = 0;
}

/* the run: text the user's message, 0 to go on with the conversation as it is (a retry) */
static int run(pi_session *s, const char *text, const char *images)
{
    int i, round;
    s->retry_next = 0;
    s->run_msg0 = s->nmsg;
    s->run_sys0 = s->nsys;
    s->sys_seen = s->nsys;
    emit_simple(s, PE_AGENT_START);
    emit_simple(s, PE_TURN_START);
    announce_system(s);
    if (text) {
        i = add_user(s, text, images);
        if (i < 0) {
            s->stop = LS_ERROR;
            cl_copy(s->err, "out of memory", sizeof(s->err));
            emit_simple(s, PE_AGENT_END);
            return s->stop;
        }
    }
    flush_next_custom(s);       /* before_agent_start's messages follow the user's */
    poll_queue(s, 0);           /* typed before the first request (pi polls at the start) */
    for (round = 0; round < PI_ROUNDS; round++) {
        stream t;
        llm_info inf;
        int k, n, stopped = 0, go, batch_term = 0;
        if (round) {
            /* pi's prepareNextTurn (compaction) runs before the pending messages go in; steering typed meanwhile is
             * polled only when the turn's poll found nothing (one-at-a-time mode delivers one a turn) */
            if (s->compact)
                s->compact(s->compact_u, s, PI_CP_TURN);
            if (!s->npend)
                poll_queue(s, 0);
            emit_simple(s, PE_TURN_START);
        }
        deliver(s);
        pi_agent_rounds++;
        s->rounds++;
        memset(&t, 0, sizeof(t));
        t.s = s;
        i = answer(s, &t);
        if (i < 0) {
            llm_acc_free(&t.acc);
            s->stop = LS_ERROR;
            cl_copy(s->err, "out of memory", sizeof(s->err));
            emit_simple(s, PE_TURN_END);
            return run_end(s, round > 0);
        }
        if (!t.started)
            emit_msg(s, PE_MESSAGE_START, i, LR_ASSISTANT);
        emit_msg(s, PE_MESSAGE_END, i, LR_ASSISTANT);
        llm_msg_info(s->msg[i], s->msg_n[i], &inf);
        s->stop = inf.stop;
        cl_copy(s->err, t.acc.failed ? t.acc.err : "", sizeof(s->err));
        if (s->attempt && inf.stop != LS_ERROR)
            emit_retry_end(s, 1, 0);        /* pi resets the retry count on the first answer that is not a failure */
        if (inf.stop == LS_ERROR || inf.stop == LS_ABORTED) {
            llm_acc_free(&t.acc);
            emit_turn_end(s, i);
            return run_end(s, 1);
        }
        n = llm_acc_tools(&t.acc);
        {
            int term, all_term = n > 0;
            for (k = 0; k < n && !stopped; k++) {
                stopped = run_call(s, llm_acc_tool(&t.acc, k), inf.stop == LS_LENGTH, &term);
                all_term = all_term && term;
            }
            batch_term = all_term && !stopped;
        }
        llm_acc_free(&t.acc);
        if (stopped) {
            s->stop = LS_ABORTED;
            cl_copy(s->err, "Operation aborted", sizeof(s->err));
        }
        go = emit_turn_end(s, i);
        if (stopped)
            return run_end(s, 0);               /* pi does not check a run the user stopped */
        /* steering is polled after the turn's tool calls and goes in at the next turn; the run goes on for it even
         * when the answer called no tool. When the run would stop, a follow-up does the same. */
        if (poll_queue(s, 0) || (n && !batch_term) || go)
            continue;
        if (poll_queue(s, 1))
            continue;
        return run_end(s, 1);
    }
    /* the round limit with messages polled and not delivered: back to the front of their queue (no key was read
     * since the poll, so the room they left is still there) */
    if (s->npend) {
        char **from = s->pend_follow ? s->follow : s->steer;
        int *from_n = s->pend_follow ? &s->nfollow : &s->nsteer;
        memmove(from + s->npend, from, sizeof(from[0]) * (size_t)*from_n);
        for (i = 0; i < s->npend; i++)
            from[i] = s->pend[i];
        *from_n += s->npend;
        s->npend = 0;
    }
    cl_copy(s->err, "stopped after 64 rounds", sizeof(s->err));
    return run_end(s, 1);
}

/* the wait before a retry, in pieces so a stop (Ctrl+C, abort_retry) cuts it: 1 when it was cut */
static int retry_wait(pi_session *s, long ms)
{
    while (ms > 0) {
        long t = ms > 50 ? 50 : ms;
        if (s->retry_stop || (s->h.poll && s->h.poll(s->h.u)))
            return 1;
        if (s->h.sleep ? s->h.sleep(s->h.u, t) : 0)
            return 1;
        ms -= t;
    }
    return s->retry_stop;
}

static int prompt_run(pi_session *s, const char *typed, const char *images)
{
    int rc;
    jw ex;
    const char *text = expand_input(s, typed, &ex);
    pi_agent_prompts++;
    s->err[0] = 0;
    s->active = 1;
    s->aborted = 0;                     /* agent-session.ts _runAgentPrompt */
    s->attempt = 0;
    s->retry_stop = 0;
    s->recover = 0;
    /* the context against the window before the user message is kept */
    if (s->compact)
        s->compact(s->compact_u, s, PI_CP_PROMPT);
    pi_ev_before_agent_start(s, text);          /* after the compaction check, before the run (agent-session.ts:2062) */
    if (s->started) {
        void (*f)(void *) = s->started;
        s->started = 0;
        f(s->started_u);
    }
    rc = run(s, text, images);
    jw_free(&ex);
    /* a failed answer pi retries after its delay; a context overflow the store compacted is asked again at once, and
     * only once (the store keeps count: its second overflow ends the run, as pi's _overflowRecoveryAttempted) */
    for (;;) {
        pi_event e;
        long delay, cap;
        int k, last;
        if (!s->retry_next) {
            if (!s->recover) {
                /* the run would settle: agent_before_settle may ask for one more request (_runBeforeSettleBoundary) */
                if (s->aborted || !pi_ev_before_settle(s))
                    break;
                rc = run(s, 0, 0);
                continue;
            }
            s->recover = 0;
            rc = run(s, 0, 0);
            continue;
        }
        delay = s->retry_base;
        cap = s->retry_max_delay;
        last = s->nmsg - 1;
        s->attempt++;
        for (k = 1; k < s->attempt && delay < cap; k++)
            delay *= 2;
        if (delay > cap)
            delay = cap;
        pi_event_init(&e, PE_RETRY_START);
        e.n1 = s->attempt;
        e.n2 = s->retry_max;
        e.n3 = delay;
        e.msg = s->err[0] ? s->err : "Unknown error";
        e.msg_n = (long)strlen(e.msg);
        pi_emit(&s->em, &e);
        /* the failed answer stays in the history but out of the context (pi's context_edit entry) */
        if (s->omit && last >= 0)
            s->omit(s->omit_u, s, last);
        s->retry_waiting = 1;
        k = retry_wait(s, delay);
        s->retry_waiting = 0;
        if (k) {
            emit_retry_end(s, 0, "Retry cancelled");
            break;
        }
        rc = run(s, 0, 0);
    }
    s->retry_stop = 0;
    s->active = 0;
    {
        pi_event e;
        pi_event_init(&e, PE_AGENT_SETTLED);
        e.aborted = s->aborted;
        pi_emit(&s->em, &e);
    }
    jw_free(&ex);
    return rc;
}

/* preflightResult("handled"): the prompt ended with no run (an extension command ran, or the input event consumed it) */
static int prompt_handled(pi_session *s)
{
    s->err[0] = 0;
    if (s->started) {
        void (*f)(void *) = s->started;
        s->started = 0;
        s->disposition = "handled";
        f(s->started_u);
        s->disposition = 0;
    }
    return 0;
}

/* agent-session.ts prompt(): a registered extension command runs first (1977-1983), then the extensions' input event (1993)
 * comes before the skill command and the prompt template expand (2005-2010; prompt_run) and before the compaction check;
 * "handled" ends the call with no run, "transform" is the new text */
int pi_prompt_images(pi_session *s, const char *text, const char *images)
{
    jw t, im;
    int rc;
    if (text && text[0] == '/' && pi_exts_run_command(&s->ext, text))
        return prompt_handled(s);
    if (!text || !s->ext.n)
        return prompt_run(s, text, images);
    jw_init(&t);
    jw_init(&im);
    jw_rawz(&t, text);
    if (images && images[0])
        jw_rawz(&im, images);
    s->ext.images = &im;
    if (pi_exts_input(&s->ext, &t)) {
        s->ext.images = 0;
        jw_free(&t);
        jw_free(&im);
        return prompt_handled(s);
    }
    s->ext.images = 0;
    rc = prompt_run(s, t.p ? t.p : "", im.n ? im.p : 0);
    jw_free(&t);
    jw_free(&im);
    return rc;
}

int pi_prompt(pi_session *s, const char *text)
{
    return pi_prompt_images(s, text, 0);
}

long pi_last_text(const pi_session *s, jw *out)
{
    int i;
    for (i = s->nmsg - 1; i >= 0; i--) {
        jv m, r, c, b, t, x;
        jit it;
        if (json_parse(s->msg[i], s->msg_n[i], &m) || !json_get(m, "role", &r) || !json_streq(r, "assistant"))
            continue;
        if (json_get(m, "content", &c) && json_type(c) == J_ARR) {
            json_iter(c, &it);
            while (json_next(&it, 0, &b))
                if (json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                    long n;
                    char *z = json_strdup(x, &n);
                    if (z) {
                        jw_raw(out, z, n);
                        free(z);
                    }
                }
        }
        break;
    }
    return out->n;
}
