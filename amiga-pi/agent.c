/* see agent.h */
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "llm/transform.h"
#include "llm/handoff.h"
#include "claude/util.h"
#include "claude/repl.h"        /* cl_key_clean */

long pi_agent_rounds;

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
    pi_emitter_init(&s->em);
    pi_exts_init(&s->ext);
    pi_subscribe(&s->em, pi_exts_event, &s->ext);
}

void pi_session_clear(pi_session *s)
{
    int i;
    for (i = 0; i < s->nmsg; i++)
        free(s->msg[i]);
    s->nmsg = 0;
    for (i = 0; i < s->nsys; i++)
        free(s->sys[i]);
    s->nsys = 0;
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
    free(s->sys);
    free(s->sys_n);
    free(s->sys_at);
    s->sys = 0;
    s->sys_n = 0;
    s->sys_at = 0;
    s->syscap = 0;
    free(s->msg);
    free(s->msg_n);
    s->msg = 0;
    s->msg_n = 0;
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
        if (!gm || !gn) {
            free(gm);
            free(gn);
            s->oom = 1;
            return -1;
        }
        if (s->nmsg) {
            memcpy(gm, s->msg, (size_t)s->nmsg * sizeof(*gm));
            memcpy(gn, s->msg_n, (size_t)s->nmsg * sizeof(*gn));
        }
        free(s->msg);
        free(s->msg_n);
        s->msg = gm;
        s->msg_n = gn;
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
    w->p = 0;
    w->n = w->cap = 0;
    return s->nmsg++;
}

int pi_session_push(pi_session *s, const char *json, long n)
{
    jw w;
    jw_init(&w);
    jw_raw(&w, json, n);
    return keep(s, &w) < 0 ? -1 : 0;
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
    pi_emit(&s->em, &e);
}

static void emit_simple(pi_session *s, int type)
{
    pi_event e;
    pi_event_init(&e, type);
    e.stop = s->stop;
    pi_emit(&s->em, &e);
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
        pi_emit(&t->s->em, &pe);
    } else if (t->started && (e->type == LE_BLOCK_START || e->type == LE_DELTA || e->type == LE_SIG_DELTA ||
                              e->type == LE_BLOCK_END)) {
        pi_event_init(&pe, PE_MESSAGE_UPDATE);
        pe.role = LR_ASSISTANT;
        pe.ae = e;
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
        jw_rawz(w, "}");
    }
    jw_rawz(w, "]");
}

int pi_req_auth(pi_session *s, llm_req *q, jw *hdr)
{
    q->key = s->key;
    q->xhead = 0;
    if (!s->cfg)
        return 0;
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
    if (llm_ant_mid_system(&s->model)) {
        /* the transcript's system messages in place (the backend reads the first as the initial one) */
        int k, j = 0, o = 0;
        if (s->declare)
            s->declare(s->declare_u, s);
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
        pm = tm;
        pn = tn;
        pc = o;
    }
    rc = llm_prepare(&s->model, pm, pn, pc, now(s), &v);
    free(tm);
    free(tn);
    if (rc) {
        llm_view_free(&v);
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
    c.msgs = v.m;
    c.msg_n = v.n;
    c.nmsg = v.count;
    c.tools = s->ntools ? tools.p : 0;
    c.tools_n = tools.n;
    c.thinking = s->thinking;
    llm_acc_init(&t->acc, s->model.api, s->model.provider);
    llm_acc_model(&t->acc, s->model.id, s->model.cost);
    llm_acc_level(&t->acc, llm_ant_effort(&s->model, s->thinking));
    k.u = t;
    k.ev = st_ev;
    memset(&q, 0, sizeof(q));
    q.pool = s->pool;
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
    jw_free(&tools);
    jw_free(&hdr);
    jw_init(&m);
    llm_acc_message(&t->acc, now(s), &m);
    i = keep(s, &m);
    jw_free(&m);
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
static int run_call(pi_session *s, const llm_ablock *b, int length_stop)
{
    pi_event e;
    jw args, content, m;
    int is_error = 0, stopped = 0, i;
    char reason[256], what[160];
    pi_tool *t;
    jw_init(&args);
    jw_init(&content);
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
    } else if (s->h.poll && s->h.poll(s->h.u)) {
        text_result(&content, "Operation aborted");
        is_error = 1;
        stopped = 1;
    } else {
        t->calls++;
        s->call_id = b->id;
        stopped = t->run(t->u, s, args.p, args.n, &content, &is_error);
        s->call_id = 0;
        if (!content.n)
            jw_rawz(&content, "[]");
        pi_exts_tool_result(&s->ext, b->id, b->name, args.p, args.n, &content, &is_error);
    }
    pi_event_init(&e, PE_TOOL_END);
    e.tool_id = b->id;
    e.tool_name = b->name;
    e.args = args.p;
    e.args_n = args.n;
    e.result = content.p;
    e.result_n = content.n;
    e.is_error = is_error;
    pi_emit(&s->em, &e);
    jw_init(&m);
    llm_m_toolresult(&m, b->id, b->name, content.p, content.n, is_error ? "{}" : 0, is_error ? 2 : 0, is_error,
                     now(s));
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

/* agent_end, then pi's check after the run when it gave an answer */
static int run_end(pi_session *s, int answered)
{
    emit_simple(s, PE_AGENT_END);
    if (answered && s->compact)
        s->compact(s->compact_u, s, PI_CP_END);
    return s->stop;
}

/* a user message (pi: a prompt's content is an array of blocks, the text first -- agent-session.js prompt) kept
 * and announced: its index, -1 out of memory */
static int add_user(pi_session *s, const char *text)
{
    jw w, c;
    int i;
    jw_init(&c);
    jw_rawz(&c, "[");
    llm_b_text(&c, text, (long)strlen(text), 0);
    jw_rawz(&c, "]");
    jw_init(&w);
    llm_m_user(&w, c.p, c.n, now(s));
    jw_free(&c);
    i = keep(s, &w);
    jw_free(&w);
    if (i >= 0) {
        emit_msg(s, PE_MESSAGE_START, i, LR_USER);
        emit_msg(s, PE_MESSAGE_END, i, LR_USER);
    }
    return i;
}

static int queue_add(char **q, int *n, const char *text)
{
    char *c = (char *)malloc(strlen(text) + 1);
    if (!c || *n == PI_QUEUE) {
        free(c);
        return -1;
    }
    strcpy(c, text);
    q[(*n)++] = c;
    return 0;
}

int pi_steer(pi_session *s, const char *text)
{
    return queue_add(s->steer, &s->nsteer, text);
}

int pi_follow_up(pi_session *s, const char *text)
{
    return queue_add(s->follow, &s->nfollow, text);
}

int pi_queue_take(pi_session *s, char **out, int max)
{
    int n = 0, i;
    for (i = 0; i < s->nsteer && n < max; i++)
        out[n++] = s->steer[i];
    s->nsteer = 0;
    for (i = 0; i < s->nfollow && n < max; i++)
        out[n++] = s->follow[i];
    s->nfollow = 0;
    return n;
}

/* agent-loop.js getSteeringMessages / getFollowUpMessages: the next queued message (all of them in "all" mode)
 * delivered as user messages; how many */
static int deliver(pi_session *s, char **q, int *n, int all)
{
    int take = all ? *n : (*n ? 1 : 0), i, got = 0;
    char *t[PI_QUEUE];
    for (i = 0; i < take; i++)
        t[i] = q[i];
    memmove(q, q + take, sizeof(q[0]) * (size_t)(*n - take));
    *n -= take;
    for (i = 0; i < take; i++) {
        if (add_user(s, t[i]) >= 0)
            got++;
        free(t[i]);
    }
    return got;
}

int pi_prompt(pi_session *s, const char *text)
{
    int i, round;
    s->err[0] = 0;
    /* the context against the window before the user message is kept */
    if (s->compact)
        s->compact(s->compact_u, s, PI_CP_PROMPT);
    emit_simple(s, PE_AGENT_START);
    emit_simple(s, PE_TURN_START);
    i = add_user(s, text);
    if (i < 0) {
        s->stop = LS_ERROR;
        cl_copy(s->err, "out of memory", sizeof(s->err));
        emit_simple(s, PE_AGENT_END);
        return s->stop;
    }
    deliver(s, s->steer, &s->nsteer, s->steer_all);     /* typed before the first request (pi polls at the start) */
    for (round = 0; round < PI_ROUNDS; round++) {
        stream t;
        llm_info inf;
        int k, n, stopped = 0;
        if (round) {
            if (s->compact)
                s->compact(s->compact_u, s, PI_CP_TURN);
            emit_simple(s, PE_TURN_START);
        }
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
        if (inf.stop == LS_ERROR || inf.stop == LS_ABORTED) {
            llm_acc_free(&t.acc);
            emit_simple(s, PE_TURN_END);
            return run_end(s, 1);
        }
        n = llm_acc_tools(&t.acc);
        for (k = 0; k < n && !stopped; k++)
            stopped = run_call(s, llm_acc_tool(&t.acc, k), inf.stop == LS_LENGTH);
        llm_acc_free(&t.acc);
        if (stopped) {
            s->stop = LS_ABORTED;
            cl_copy(s->err, "Operation aborted", sizeof(s->err));
        }
        emit_simple(s, PE_TURN_END);
        if (stopped)
            return run_end(s, 0);               /* pi does not check a run the user stopped */
        /* steering goes in after the turn's tool calls, before the next request; the run goes on for it even
         * when the answer called no tool. At the end of the run, a follow-up does the same. */
        if (deliver(s, s->steer, &s->nsteer, s->steer_all) || n)
            continue;
        if (deliver(s, s->follow, &s->nfollow, s->follow_all))
            continue;
        return run_end(s, 1);
    }
    cl_copy(s->err, "stopped after 64 rounds", sizeof(s->err));
    return run_end(s, 1);
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
