/* see agent.h */
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "llm/transform.h"
#include "claude/util.h"

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
    pi_emitter_init(&s->em);
    pi_exts_init(&s->ext);
    pi_subscribe(&s->em, pi_exts_event, &s->ext);
}

void pi_session_free(pi_session *s)
{
    int i;
    for (i = 0; i < s->nmsg; i++)
        free(s->msg[i]);
    free(s->msg);
    free(s->msg_n);
    s->msg = 0;
    s->msg_n = 0;
    s->nmsg = s->cap = 0;
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
    s->msg[s->nmsg] = w->p;
    s->msg_n[s->nmsg] = w->n;
    w->p = 0;
    w->n = w->cap = 0;
    return s->nmsg++;
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
static void tools_json(const pi_session *s, jw *w)
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

/* the request and the answer kept: the answer's index, -1 out of memory */
static int answer(pi_session *s, stream *t)
{
    llm_view v;
    llm_ctx c;
    llm_req q;
    llm_sink k;
    jw tools, m;
    int i;
    if (llm_transform((const char *const *)s->msg, s->msg_n, s->nmsg, now(s), &v)) {
        llm_view_free(&v);
        s->oom = 1;
        return -1;
    }
    jw_init(&tools);
    if (s->ntools)
        tools_json(s, &tools);
    memset(&c, 0, sizeof(c));
    c.model = &s->model;
    c.system = s->system;
    c.msgs = v.m;
    c.msg_n = v.n;
    c.nmsg = v.count;
    c.tools = s->ntools ? tools.p : 0;
    c.tools_n = tools.n;
    llm_acc_init(&t->acc, s->model.api, s->model.provider);
    k.u = t;
    k.ev = st_ev;
    memset(&q, 0, sizeof(q));
    q.pool = s->pool;
    q.model = &s->model;
    q.be = llm_backend_of(s->model.api);
    q.ctx = &c;
    q.key = s->key;
    q.user_agent = s->user_agent;
    q.h = s->h;
    q.sink = &k;
    if (!q.be) {
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
        stopped = t->run(t->u, s, args.p, args.n, &content, &is_error);
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

int pi_prompt(pi_session *s, const char *text)
{
    jw w, c;
    int i, round;
    s->err[0] = 0;
    emit_simple(s, PE_AGENT_START);
    emit_simple(s, PE_TURN_START);
    jw_init(&c);
    jw_strz(&c, text);
    jw_init(&w);
    llm_m_user(&w, c.p, c.n, now(s));
    jw_free(&c);
    i = keep(s, &w);
    jw_free(&w);
    if (i < 0) {
        s->stop = LS_ERROR;
        cl_copy(s->err, "out of memory", sizeof(s->err));
        emit_simple(s, PE_AGENT_END);
        return s->stop;
    }
    emit_msg(s, PE_MESSAGE_START, i, LR_USER);
    emit_msg(s, PE_MESSAGE_END, i, LR_USER);
    for (round = 0; round < PI_ROUNDS; round++) {
        stream t;
        llm_info inf;
        int k, n, stopped = 0;
        if (round)
            emit_simple(s, PE_TURN_START);
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
            emit_simple(s, PE_AGENT_END);
            return s->stop;
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
            emit_simple(s, PE_AGENT_END);
            return s->stop;
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
        if (!n || stopped) {
            emit_simple(s, PE_AGENT_END);
            return s->stop;
        }
    }
    cl_copy(s->err, "stopped after 64 rounds", sizeof(s->err));
    emit_simple(s, PE_AGENT_END);
    return s->stop;
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
