/* see mode_rpc.h */
#include <stdlib.h>
#include <string.h>
#include "mode_rpc.h"
#include "ext_events.h"
#include "mode_json.h"
#include "export.h"
#include "compact.h"
#include "templates.h"
#include "claude/util.h"
#include "claude/tools.h"       /* ASK_* */
#include "claude/path.h"

#define IN_CAP 4096L
#define IN_MAX (4L * 1024 * 1024)

typedef struct pi_rpc pi_rpc;
struct pi_rpc {
    pi_rt *rt;
    pi_session *s;
    pi_store *st;
    cl_io *io;
    pi_jout jout;
    char *in;                   /* bytes read, not yet a line */
    long in_n, in_cap;
    int eof;
    int (*poll)(void *u);       /* the platform's break check (Ctrl+C) */
    int in_poll;                /* the pump is running */
    int abort_req;              /* abort: the run is to stop */
    int abort_wait;             /* its response waits for the run to settle */
    int retry_resp;             /* abort_retry cut a retry wait: its response follows the auto_retry_end event (pi writes them so) */
    jw retry_id;                /* its id (raw JSON), empty none */
    jw abort_id;                /* the abort command's id (raw JSON), empty none */
    int bash_abort, in_bash;
    int compacting;
    char ui_id[48];             /* the dialog the agent waits on */
    int ui_wait, ui_got;
    jw ui_resp;                 /* its response (the JSON object) */
    unsigned long seed;
    jw out;
};

static pi_rpc *g_rpc;

/* ---- records out ---- */

static void put_line(pi_rpc *r, const jw *w)
{
    if (!w->oom && w->p)
        pi_jout_line(r->io, w->p, w->n);
}

/* {"id"?,"type":"response","command","success",...}; id a raw JSON value (0 none), data raw JSON (0 none) */
static void respond(pi_rpc *r, const char *id, long idn, const char *cmd, int ok, const char *data, long dn, const char *err)
{
    jw w;
    jw_init(&w);
    jw_rawz(&w, "{");
    if (id) {
        jw_rawz(&w, "\"id\":");
        jw_raw(&w, id, idn);
        jw_rawz(&w, ",");
    }
    jw_rawz(&w, "\"type\":\"response\"");
    if (cmd) {
        jw_rawz(&w, ",\"command\":");
        jw_strz(&w, cmd);
    }
    jw_rawz(&w, ok ? ",\"success\":true" : ",\"success\":false");
    if (ok && data) {
        jw_rawz(&w, ",\"data\":");
        jw_raw(&w, data, dn);
    } else if (!ok) {
        jw_rawz(&w, ",\"error\":");
        jw_strz(&w, err ? err : "");
    }
    jw_rawz(&w, "}");
    put_line(r, &w);
    jw_free(&w);
}

/* ---- the input ---- */

/* the next line of input (no line feed, a carriage return before it cut) into *line: 1 a line, 0 none within wait_ms, -1
 * the end of the input */
static int next_line(pi_rpc *r, long wait_ms, jw *line)
{
    for (;;) {
        long k;
        for (k = 0; k < r->in_n && r->in[k] != '\n'; k++)
            ;
        if (k < r->in_n || (r->eof && r->in_n)) {
            long len = k, rest = k < r->in_n ? k + 1 : k;
            if (len && r->in[len - 1] == '\r')
                len--;
            jw_reset(line);
            jw_raw(line, r->in, len);
            memmove(r->in, r->in + rest, (size_t)(r->in_n - rest));
            r->in_n -= rest;
            return 1;
        }
        if (r->eof)
            return -1;
        if (!r->io->read) {
            /* line mode: a read blocks, so only when the caller waits */
            long got;
            if (wait_ms == 0)
                return 0;
            if (r->in_cap - r->in_n < IN_CAP + 2) {
                char *g = (char *)realloc(r->in, (size_t)(r->in_cap + IN_CAP * 4));
                if (!g)
                    return -1;
                r->in = g;
                r->in_cap += IN_CAP * 4;
            }
            got = r->io->read_line(r->io->u, r->in + r->in_n, r->in_cap - r->in_n - 2);
            if (got < 0) {
                r->eof = 1;
                continue;
            }
            r->in_n += got;
            r->in[r->in_n++] = '\n';
        } else {
            long got;
            if (r->in_cap - r->in_n < IN_CAP) {
                char *g;
                if (r->in_cap >= IN_MAX)
                    return -1;
                g = (char *)realloc(r->in, (size_t)(r->in_cap * 2));
                if (!g)
                    return -1;
                r->in = g;
                r->in_cap *= 2;
            }
            got = r->io->read(r->io->u, r->in + r->in_n, r->in_cap - r->in_n, wait_ms);
            if (got < 0)
                r->eof = 1;
            else if (got == 0)
                return 0;
            else
                r->in_n += got;
        }
    }
}

static void handle_line(pi_rpc *r, const char *p, long n);

/* the commands that arrived while the agent works, handled where it waits */
static void pump(pi_rpc *r)
{
    jw line;
    if (r->in_poll)
        return;
    r->in_poll = 1;
    jw_init(&line);
    while (next_line(r, 0, &line) == 1)
        handle_line(r, line.p, line.n);
    jw_free(&line);
    r->in_poll = 0;
}

static int rpc_poll(void *u)
{
    pi_rpc *r = g_rpc;
    int brk = r->poll ? r->poll(u) : 0;
    pump(r);
    if (brk && r->s->active)
        r->s->aborted = 1;      /* Ctrl+C is session.abort() of the run */
    return brk || r->abort_req || (r->in_bash && r->bash_abort);
}

/* ---- JSON pieces ---- */

/* v as a string into a malloc'ed copy (0 when it is not a string) */
static char *str_of(jv obj, const char *key, long *n)
{
    jv v;
    if (!json_get(obj, key, &v) || json_type(v) != J_STR)
        return 0;
    return json_strdup(v, n);
}

static void levels_json(const llm_model *m, jw *w)
{
    unsigned av = llm_think_levels(m);
    int i, k = 0;
    jw_rawz(w, "[");
    for (i = 0; i < LT_N; i++)
        if (av & (1u << i)) {
            if (k++)
                jw_rawz(w, ",");
            jw_strz(w, llm_think_name(i));
        }
    jw_rawz(w, "]");
}

static int message_count(const pi_session *s)
{
    return s->nmsg + s->nsys;
}

static void state_json(pi_rpc *r, jw *w)
{
    pi_session *s = r->s;
    pi_s3 *t = &r->st->s3;
    char nm[200];
    jw_rawz(w, "{\"model\":");
    pi_model_json(&s->model, w);
    jw_rawz(w, ",\"thinkingLevel\":");
    jw_strz(w, llm_think_name(s->thinking));
    jw_rawz(w, s->active ? ",\"isStreaming\":true" : ",\"isStreaming\":false");
    jw_rawz(w, r->compacting ? ",\"isCompacting\":true" : ",\"isCompacting\":false");
    jw_rawz(w, s->steer_all ? ",\"steeringMode\":\"all\"" : ",\"steeringMode\":\"one-at-a-time\"");
    jw_rawz(w, s->follow_all ? ",\"followUpMode\":\"all\"" : ",\"followUpMode\":\"one-at-a-time\"");
    if (t->persist && t->file[0]) {
        jw_rawz(w, ",\"sessionFile\":");
        jw_strz(w, t->file);
    }
    jw_rawz(w, ",\"sessionId\":");
    jw_strz(w, t->id);
    if (s3_name(t, nm, sizeof(nm))) {
        jw_rawz(w, ",\"sessionName\":");
        jw_strz(w, nm);
    }
    jw_rawz(w, r->st->cp_enabled ? ",\"autoCompactionEnabled\":true" : ",\"autoCompactionEnabled\":false");
    jw_rawz(w, ",\"messageCount\":");
    jw_long(w, message_count(s));
    jw_rawz(w, ",\"pendingMessageCount\":");
    jw_long(w, pi_queue_len(s, 0) + pi_queue_len(s, 1));
    jw_rawz(w, "}");
}

/* a decimal number of tokens * 100 / window with up to 10 digits after the point, trailing zeros cut */
static void percent_json(jw *w, long tokens, long window)
{
    long whole = tokens * 100 / window, rem = tokens * 100 % window;
    char frac[16];
    int i, n = 0;
    jw_long(w, whole);
    for (i = 0; i < 10 && rem; i++) {
        rem *= 10;
        frac[n++] = (char)('0' + rem / window);
        rem %= window;
    }
    while (n && frac[n - 1] == '0')
        n--;
    if (n) {
        jw_rawz(w, ".");
        jw_raw(w, frac, n);
    }
}

static void add_usage(llm_usage *tot, jv u)
{
    llm_usage x;
    int i;
    llm_r_usage(u, &x);
    tot->input += x.input;
    tot->output += x.output;
    tot->cache_read += x.cache_read;
    tot->cache_write += x.cache_write;
    for (i = 0; i < 4; i++)
        tot->cost[i] += x.cost[i];
}

static void stats_json(pi_rpc *r, jw *w)
{
    pi_session *s = r->s;
    pi_s3 *t = &r->st->s3;
    llm_usage tot;
    long users = 0, assts = 0, calls = 0, results = 0, msgs = 0;
    int i;
    pi_ctx c;
    long tokens = -1;
    memset(&tot, 0, sizeof(tot));
    for (i = 0; i < t->n; i++) {
        long n;
        const char *x;
        jv o, m, v, u;
        int ty = t->e[i].type;
        if (ty != S3_USAGE && ty != S3_COMPACTION && ty != S3_BRANCH_SUMMARY && ty != S3_MESSAGE)
            continue;
        x = s3_text(t, i, &n);
        if (!x || json_parse(x, n, &o))
            continue;
        if ((ty == S3_USAGE || ty == S3_COMPACTION || ty == S3_BRANCH_SUMMARY) && json_get(o, "usage", &u) && json_type(u) == J_OBJ)
            add_usage(&tot, u);
        if (ty != S3_MESSAGE)
            continue;
        msgs++;
        if (t->e[i].role == S3R_USER)
            users++;
        else if (t->e[i].role == S3R_TOOLRESULT) {
            results++;
            if (json_get(o, "message", &m) && json_get(m, "usage", &u) && json_type(u) == J_OBJ)
                add_usage(&tot, u);
        } else if (t->e[i].role == S3R_ASSISTANT && json_get(o, "message", &m)) {
            assts++;
            if (json_get(m, "content", &v) && json_type(v) == J_ARR) {
                jit it;
                jv b, ty2;
                json_iter(v, &it);
                while (json_next(&it, 0, &b))
                    if (json_get(b, "type", &ty2) && json_streq(ty2, "toolCall"))
                        calls++;
            }
            if (json_get(m, "usage", &u) && json_type(u) == J_OBJ)
                add_usage(&tot, u);
        }
    }
    jw_rawz(w, "{");
    if (t->persist && t->file[0]) {
        jw_rawz(w, "\"sessionFile\":");
        jw_strz(w, t->file);
        jw_rawz(w, ",");
    }
    jw_rawz(w, "\"sessionId\":");
    jw_strz(w, t->id);
    jw_rawz(w, ",\"userMessages\":");
    jw_long(w, users);
    jw_rawz(w, ",\"assistantMessages\":");
    jw_long(w, assts);
    jw_rawz(w, ",\"toolCalls\":");
    jw_long(w, calls);
    jw_rawz(w, ",\"toolResults\":");
    jw_long(w, results);
    jw_rawz(w, ",\"totalMessages\":");
    jw_long(w, msgs);
    jw_rawz(w, ",\"tokens\":{\"input\":");
    jw_long(w, tot.input);
    jw_rawz(w, ",\"output\":");
    jw_long(w, tot.output);
    jw_rawz(w, ",\"cacheRead\":");
    jw_long(w, tot.cache_read);
    jw_rawz(w, ",\"cacheWrite\":");
    jw_long(w, tot.cache_write);
    jw_rawz(w, ",\"total\":");
    jw_long(w, tot.input + tot.output + tot.cache_read + tot.cache_write);
    jw_rawz(w, "},\"cost\":");
    llm_w_micro(w, tot.cost[0] + tot.cost[1] + tot.cost[2] + tot.cost[3]);
    if (s->model.context_window > 0) {
        ctx_init(&c);
        if (!ctx_build(t, t->leaf, &c)) {
            tokens = cp_context_usage(t, t->leaf, &c);
            ctx_free(&c);
        }
        jw_rawz(w, ",\"contextUsage\":{\"tokens\":");
        if (tokens < 0)
            jw_rawz(w, "null");
        else
            jw_long(w, tokens);
        jw_rawz(w, ",\"contextWindow\":");
        jw_long(w, s->model.context_window);
        jw_rawz(w, ",\"percent\":");
        if (tokens < 0)
            jw_rawz(w, "null");
        else
            percent_json(w, tokens, s->model.context_window);
        jw_rawz(w, "}");
    }
    jw_rawz(w, "}");
}

/* the entry's label with the time of the label entry that set it: 1 and filled, 0 none */
static int label_of(pi_s3 *t, int i, char *label, long cap, char *ts, long tcap)
{
    int j;
    for (j = t->n - 1; j >= 0; j--)
        if (t->e[j].type == S3_LABEL && t->e[j].target == i) {
            long n;
            const char *x = s3_text(t, j, &n);
            jv o, v;
            label[0] = 0;
            ts[0] = 0;
            if (!x || json_parse(x, n, &o))
                return 0;
            if (!json_get(o, "label", &v) || json_type(v) != J_STR || json_str(v, label, cap) <= 0)
                return 0;
            if (json_get(o, "timestamp", &v) && json_type(v) == J_STR)
                json_str(v, ts, tcap);
            return 1;
        }
    return 0;
}

/* pi's getTree: every entry as {entry, children, label?, labelTimestamp?}, children in file order; iterative, a session is
 * a long chain */
static int tree_json(pi_s3 *t, jw *w)
{
    int *next, *stack, *kid, i, depth = 0;
    char label[200], ts[40];
    if (!t->n) {
        jw_rawz(w, "[]");
        return 0;
    }
    /* first child and next sibling lists, in file order */
    kid = (int *)malloc((size_t)t->n * sizeof(int));
    next = (int *)malloc((size_t)t->n * sizeof(int));
    stack = (int *)malloc((size_t)(t->n + 1) * sizeof(int));
    if (!kid || !next || !stack) {
        free(kid);
        free(next);
        free(stack);
        return -1;
    }
    {
        int *last = (int *)malloc((size_t)t->n * sizeof(int));
        int lastroot = -1, firstroot = -1;
        if (!last) {
            free(kid);
            free(next);
            free(stack);
            return -1;
        }
        for (i = 0; i < t->n; i++) {
            kid[i] = -1;
            next[i] = -1;
            last[i] = -1;
        }
        for (i = 0; i < t->n; i++) {
            int p = t->e[i].parent;
            if (p < 0) {
                if (lastroot >= 0)
                    next[lastroot] = i;
                else
                    firstroot = i;
                lastroot = i;
            } else {
                if (last[p] >= 0)
                    next[last[p]] = i;
                else
                    kid[p] = i;
                last[p] = i;
            }
        }
        free(last);
        jw_rawz(w, "[");
        /* walk: stack holds the node whose children are being written; cur is the node to open next */
        {
            int cur = firstroot;
            while (cur >= 0 || depth > 0) {
                if (cur >= 0) {
                    long n;
                    const char *x = s3_text(t, cur, &n);
                    jw_rawz(w, "{\"entry\":");
                    jw_raw(w, x ? x : "null", x ? n : 4);
                    jw_rawz(w, ",\"children\":[");
                    stack[depth++] = cur;
                    cur = kid[cur];
                } else {
                    int done = stack[--depth];
                    jw_rawz(w, "]");
                    if (label_of(t, done, label, sizeof(label), ts, sizeof(ts))) {
                        jw_rawz(w, ",\"label\":");
                        jw_strz(w, label);
                        if (ts[0]) {
                            jw_rawz(w, ",\"labelTimestamp\":");
                            jw_strz(w, ts);
                        }
                    }
                    jw_rawz(w, "}");
                    cur = next[done];
                    if (cur >= 0)
                        jw_rawz(w, ",");
                }
            }
        }
        jw_rawz(w, "]");
    }
    free(kid);
    free(next);
    free(stack);
    return 0;
}

/* ---- extension UI ---- */

static void ui_open(pi_rpc *r, jw *w, const char *method, char *id)
{
    cl_uuid(&r->seed, id);
    jw_rawz(w, "{\"type\":\"extension_ui_request\",\"id\":");
    jw_strz(w, id);
    jw_rawz(w, ",\"method\":");
    jw_strz(w, method);
}

/* the client's answer to dialog id: the JSON object in r->ui_resp, 1; 0 when the time ran out or the input ended */
static int dialog_wait(pi_rpc *r, const char *id, long timeout_ms)
{
    unsigned long t0 = r->io->ms ? r->io->ms(r->io->u) : 0;
    jw line;
    cl_copy(r->ui_id, id, sizeof(r->ui_id));
    r->ui_wait = 1;
    r->ui_got = 0;
    jw_reset(&r->ui_resp);
    jw_init(&line);
    while (!r->ui_got) {
        int rc = next_line(r, 20, &line);
        if (rc < 0)
            break;
        if (rc == 1)
            handle_line(r, line.p, line.n);
        else if (r->io->ms && timeout_ms > 0 && (long)(r->io->ms(r->io->u) - t0) >= timeout_ms)
            break;
        if (!r->io->read && timeout_ms > 0 && r->io->ms && (long)(r->io->ms(r->io->u) - t0) >= timeout_ms)
            break;
    }
    jw_free(&line);
    r->ui_wait = 0;
    return r->ui_got;
}

static int cancelled(const jw *resp)
{
    jv o, v;
    return !resp->n || json_parse(resp->p, resp->n, &o) || (json_get(o, "cancelled", &v) && json_type(v) == J_TRUE);
}

static int rpc_ui_select(void *u, const char *title, const char *const *options, int n, long timeout_ms, char *value, long cap)
{
    pi_rpc *r = (pi_rpc *)u;
    jw w;
    char id[48];
    int i, ok = 0;
    jv o, v;
    jw_init(&w);
    ui_open(r, &w, "select", id);
    jw_rawz(&w, ",\"title\":");
    jw_strz(&w, title);
    jw_rawz(&w, ",\"options\":[");
    for (i = 0; i < n; i++) {
        if (i)
            jw_rawz(&w, ",");
        jw_strz(&w, options[i]);
    }
    jw_rawz(&w, "]");
    if (timeout_ms > 0) {
        jw_rawz(&w, ",\"timeout\":");
        jw_long(&w, timeout_ms);
    }
    jw_rawz(&w, "}");
    put_line(r, &w);
    jw_free(&w);
    if (dialog_wait(r, id, timeout_ms) && !cancelled(&r->ui_resp) && !json_parse(r->ui_resp.p, r->ui_resp.n, &o) &&
        json_get(o, "value", &v) && json_type(v) == J_STR) {
        json_str(v, value, cap);
        ok = 1;
    }
    return ok;
}

static int rpc_ui_confirm(void *u, const char *title, const char *message, long timeout_ms)
{
    pi_rpc *r = (pi_rpc *)u;
    jw w;
    char id[48];
    jv o, v;
    int yes = 0;
    jw_init(&w);
    ui_open(r, &w, "confirm", id);
    jw_rawz(&w, ",\"title\":");
    jw_strz(&w, title);
    jw_rawz(&w, ",\"message\":");
    jw_strz(&w, message ? message : "");
    if (timeout_ms > 0) {
        jw_rawz(&w, ",\"timeout\":");
        jw_long(&w, timeout_ms);
    }
    jw_rawz(&w, "}");
    put_line(r, &w);
    jw_free(&w);
    if (dialog_wait(r, id, timeout_ms) && !cancelled(&r->ui_resp) && !json_parse(r->ui_resp.p, r->ui_resp.n, &o) &&
        json_get(o, "confirmed", &v))
        yes = json_type(v) == J_TRUE;
    return yes;
}

static int rpc_ui_input(void *u, const char *title, const char *placeholder, long timeout_ms, char *value, long cap)
{
    pi_rpc *r = (pi_rpc *)u;
    jw w;
    char id[48];
    jv o, v;
    int ok = 0;
    jw_init(&w);
    ui_open(r, &w, "input", id);
    jw_rawz(&w, ",\"title\":");
    jw_strz(&w, title);
    if (placeholder) {
        jw_rawz(&w, ",\"placeholder\":");
        jw_strz(&w, placeholder);
    }
    if (timeout_ms > 0) {
        jw_rawz(&w, ",\"timeout\":");
        jw_long(&w, timeout_ms);
    }
    jw_rawz(&w, "}");
    put_line(r, &w);
    jw_free(&w);
    if (dialog_wait(r, id, timeout_ms) && !cancelled(&r->ui_resp) && !json_parse(r->ui_resp.p, r->ui_resp.n, &o) &&
        json_get(o, "value", &v) && json_type(v) == J_STR) {
        json_str(v, value, cap);
        ok = 1;
    }
    return ok;
}

static void rpc_ui_notify(void *u, const char *message, const char *type)
{
    pi_rpc *r = (pi_rpc *)u;
    jw w;
    char id[48];
    jw_init(&w);
    ui_open(r, &w, "notify", id);
    jw_rawz(&w, ",\"message\":");
    jw_strz(&w, message);
    if (type) {
        jw_rawz(&w, ",\"notifyType\":");
        jw_strz(&w, type);
    }
    jw_rawz(&w, "}");
    put_line(r, &w);
    jw_free(&w);
}

/* what an extension's ctx.ui reaches while RPC mode runs (ext.h pi_ext_ui): requests out, the client's responses in */
static pi_ext_ui rpc_ui = { 0, rpc_ui_select, rpc_ui_confirm, rpc_ui_input, rpc_ui_notify };

/* the permission gate's question (ext_gate.h) as a select */
static int rpc_ask(void *u, int tool, const char *name, const char *what, int outside)
{
    pi_rpc *r = (pi_rpc *)u;
    static const char *const opt[] = { "Yes", "Yes, for this session", "Yes, and do not ask again in this project", "No" };
    char title[400], pick[80];
    int i;
    (void)tool;
    (void)outside;
    cl_copy(title, "Allow ", sizeof(title));
    cl_cat(title, name, sizeof(title));
    if (what && what[0]) {
        cl_cat(title, ": ", sizeof(title));
        cl_cat(title, what, sizeof(title));
    }
    cl_cat(title, "?", sizeof(title));
    if (!rpc_ui_select(r, title, opt, 4, 0, pick, sizeof(pick)))
        return ASK_NO;
    for (i = 0; i < 4; i++)
        if (!strcmp(pick, opt[i]))
            return i == 0 ? ASK_ONCE : i == 1 ? ASK_SESSION : i == 2 ? ASK_PROJECT : ASK_NO;
    return ASK_NO;
}

/* ---- commands ---- */

typedef struct cmd {
    pi_rpc *r;
    const char *id;             /* raw JSON of the id, 0 none */
    long idn;
    const char *type;
    jv obj;
} cmd;

static void ok(const cmd *c, const char *data)
{
    respond(c->r, c->id, c->idn, c->type, 1, data, data ? (long)strlen(data) : 0, 0);
}

static void okw(const cmd *c, const jw *data)
{
    respond(c->r, c->id, c->idn, c->type, 1, data->p, data->n, 0);
}

static void fail(const cmd *c, const char *msg)
{
    respond(c->r, c->id, c->idn, c->type, 0, 0, 0, msg);
}

/* the text of the user's reply to a command that wants an idle agent */
static int busy(const cmd *c)
{
    if (c->r->s->active) {
        fail(c, "Agent is already processing");
        return 1;
    }
    return 0;
}

static void prompt_started(void *u)
{
    const cmd *c = (const cmd *)u;
    ok(c, c->r->s->disposition ? "{\"disposition\":\"handled\"}" : "{\"disposition\":\"started\"}");
}

static void cmd_prompt(const cmd *c)
{
    pi_rpc *r = c->r;
    pi_session *s = r->s;
    long n;
    char *msg = str_of(c->obj, "message", &n), *sb;
    long bn;
    if (!msg) {
        fail(c, "Missing message");
        return;
    }
    sb = str_of(c->obj, "streamingBehavior", &bn);
    if (s->active) {
        int rc = -1;
        if (sb && !strcmp(sb, "steer"))
            rc = pi_steer(s, msg);
        else if (sb && !strcmp(sb, "followUp"))
            rc = pi_follow_up(s, msg);
        else {
            fail(c, "Agent is already processing. Specify streamingBehavior ('steer' or 'followUp') to queue the message.");
            free(msg);
            free(sb);
            return;
        }
        if (rc)
            fail(c, "The queue is full");
        else
            ok(c, "{\"disposition\":\"queued\"}");
        free(msg);
        free(sb);
        return;
    }
    free(sb);
    s->started = prompt_started;        /* the response follows the check before the prompt, as pi's preflightResult */
    s->started_u = (void *)c;
    s->ext.source = "rpc";              /* the input event's InputSource (rpc-mode.ts: source: "rpc") */
    pi_prompt(s, msg);
    s->ext.source = 0;
    free(msg);
    r->abort_req = 0;
    if (r->abort_wait) {
        r->abort_wait = 0;
        respond(r, r->abort_id.n ? r->abort_id.p : 0, r->abort_id.n, "abort", 1, 0, 0, 0);
    }
}

static void cmd_queue(const cmd *c, int follow)
{
    long n;
    char *msg = str_of(c->obj, "message", &n);
    int rc;
    if (!msg) {
        fail(c, "Missing message");
        return;
    }
    rc = follow ? pi_follow_up(c->r->s, msg) : pi_steer(c->r->s, msg);
    free(msg);
    if (rc)
        fail(c, "The queue is full");
    else
        ok(c, "{\"disposition\":\"queued\"}");
}

static void cmd_abort(const cmd *c)
{
    pi_rpc *r = c->r;
    if (!r->s->active) {
        ok(c, 0);
        return;
    }
    r->abort_req = 1;
    r->s->aborted = 1;          /* agent-session.ts abort(): _agentRunAbortRequested while a run is active */
    r->abort_wait = 1;
    jw_reset(&r->abort_id);
    if (c->id)
        jw_raw(&r->abort_id, c->id, c->idn);
}

static void cmd_clear_queue(const cmd *c)
{
    pi_session *s = c->r->s;
    jw d;
    char *out[2 * PI_QUEUE];
    int n, i;
    jw_init(&d);
    jw_rawz(&d, "{\"steering\":");
    pi_queue_json(s, 0, &d);
    jw_rawz(&d, ",\"followUp\":");
    pi_queue_json(s, 1, &d);
    jw_rawz(&d, "}");
    n = pi_queue_take(s, out, 2 * PI_QUEUE);
    for (i = 0; i < n; i++)
        free(out[i]);
    okw(c, &d);
    jw_free(&d);
}

/* rpc-mode.ts:680-710 get_commands: the extension commands (Phase L), the prompt templates, then the skills, in that order */
static void cmd_get_commands(const cmd *c)
{
    const pi_tmpls *ts = c->r->rt ? c->r->rt->tmpls : 0;
    jw d;
    int i;
    jw_init(&d);
    jw_rawz(&d, "{\"commands\":[");
    {
        pi_ext_rcmd *xc = (pi_ext_rcmd *)malloc(sizeof(*xc) * PI_EXT_CMDS);
        int nx = xc ? pi_exts_commands(&c->r->s->ext, xc, PI_EXT_CMDS) : 0;
        for (i = 0; i < nx; i++) {
            const pi_ext *o = xc[i].c->owner;
            jw_rawz(&d, i ? ",{\"name\":" : "{\"name\":");
            jw_strz(&d, xc[i].invocation);
            if (xc[i].c->description) {
                jw_rawz(&d, ",\"description\":");
                jw_strz(&d, xc[i].c->description);
            }
            /* the sourceInfo of an extension found in a default directory: "auto" of its scope (loader.ts createSourceInfo) */
            jw_rawz(&d, ",\"source\":\"extension\",\"sourceInfo\":{\"path\":");
            jw_strz(&d, o->path ? o->path : "");
            jw_rawz(&d, ",\"source\":\"auto\",\"scope\":");
            jw_strz(&d, o->scope ? o->scope : "temporary");
            jw_rawz(&d, ",\"origin\":\"top-level\"");
            if (o->base_dir) {
                jw_rawz(&d, ",\"baseDir\":");
                jw_strz(&d, o->base_dir);
            }
            jw_rawz(&d, "}}");
        }
        free(xc);
        if (nx && ((ts && ts->n) || (c->r->s->res && c->r->s->res->skills.n)))
            jw_raw(&d, ",", 1);
    }
    if (ts)
        pi_tmpls_commands(ts, &d);
    if (c->r->s->res && c->r->s->res->skills.n) {
        if (ts && ts->n)
            jw_raw(&d, ",", 1);
        pi_skills_commands(&c->r->s->res->skills, &d);
    }
    jw_rawz(&d, "]}");
    okw(c, &d);
    jw_free(&d);
}

static void cmd_new_session(const cmd *c)
{
    pi_rpc *r = c->r;
    char *parent;
    long n;
    int rc;
    if (busy(c))
        return;
    parent = str_of(c->obj, "parentSession", &n);
    rc = pi_store_new_session(r->st, parent);
    if (rc < 0)
        fail(c, r->st->err);
    else
        ok(c, rc ? "{\"cancelled\":true}" : "{\"cancelled\":false}");
    free(parent);
}

static void cmd_get_state(const cmd *c)
{
    jw d;
    jw_init(&d);
    state_json(c->r, &d);
    okw(c, &d);
    jw_free(&d);
}

static void cmd_get_messages(const cmd *c)
{
    jw d;
    jw_init(&d);
    jw_rawz(&d, "{\"messages\":");
    pi_messages_json(c->r->s, 0, 0, &d);
    jw_rawz(&d, "}");
    okw(c, &d);
    jw_free(&d);
}

static void cmd_set_model(const cmd *c)
{
    pi_rpc *r = c->r;
    pi_models *ms = r->rt->models;
    long pn, in;
    char *prov = str_of(c->obj, "provider", &pn), *id = str_of(c->obj, "modelId", &in);
    const llm_model *m = prov && id ? llm_reg_find(&ms->reg, prov, id) : 0;
    char err[200];
    if (!m || !pi_model_usable(ms, m)) {
        cl_copy(err, "Model not found: ", sizeof(err));
        cl_cat(err, prov ? prov : "", sizeof(err));
        cl_cat(err, "/", sizeof(err));
        cl_cat(err, id ? id : "", sizeof(err));
        fail(c, err);
    } else if (pi_model_switch(ms, r->s, m, err, sizeof(err)))
        fail(c, err);
    else {
        jw d;
        jw_init(&d);
        pi_model_json(&r->s->model, &d);
        okw(c, &d);
        jw_free(&d);
    }
    free(prov);
    free(id);
}

static void cmd_cycle_model(const cmd *c)
{
    pi_rpc *r = c->r;
    char msg[200];
    int rc = pi_model_cycle(r->rt->models, r->s, 1, msg, sizeof(msg));
    if (rc < 0)
        fail(c, msg);
    else if (rc == 0)
        ok(c, "null");
    else {
        jw d;
        jw_init(&d);
        jw_rawz(&d, "{\"model\":");
        pi_model_json(&r->s->model, &d);
        jw_rawz(&d, ",\"thinkingLevel\":");
        jw_strz(&d, llm_think_name(r->s->thinking));
        jw_rawz(&d, r->rt->models->scope ? ",\"isScoped\":true}" : ",\"isScoped\":false}");
        okw(c, &d);
        jw_free(&d);
    }
}

static void cmd_get_available_models(const cmd *c)
{
    pi_models *ms = c->r->rt->models;
    jw d;
    int i, k = 0;
    jw_init(&d);
    jw_rawz(&d, "{\"models\":[");
    for (i = 0; i < ms->reg.n; i++)
        if (pi_model_usable(ms, &ms->reg.m[i])) {
            if (k++)
                jw_rawz(&d, ",");
            pi_model_json(&ms->reg.m[i], &d);
        }
    jw_rawz(&d, "]}");
    okw(c, &d);
    jw_free(&d);
}

static void cmd_set_thinking_level(const cmd *c)
{
    long n;
    char *lv = str_of(c->obj, "level", &n);
    int level = lv ? llm_think_of(lv) : -1;
    if (level < 0)
        fail(c, "Invalid thinking level");
    else if (pi_thinking_set(c->r->rt->models, c->r->s, level) < 0)
        fail(c, "the thinking level change could not be recorded");
    else
        ok(c, 0);
    free(lv);
}

static void cmd_cycle_thinking_level(const cmd *c)
{
    char msg[200];
    int rc = pi_thinking_cycle(c->r->rt->models, c->r->s, msg, sizeof(msg));
    if (rc < 0)
        fail(c, msg);
    else if (rc == 0)
        ok(c, "null");
    else {
        jw d;
        jw_init(&d);
        jw_rawz(&d, "{\"level\":");
        jw_strz(&d, llm_think_name(c->r->s->thinking));
        jw_rawz(&d, "}");
        okw(c, &d);
        jw_free(&d);
    }
}

static void cmd_get_available_thinking_levels(const cmd *c)
{
    jw d;
    jw_init(&d);
    jw_rawz(&d, "{\"levels\":");
    levels_json(&c->r->s->model, &d);
    jw_rawz(&d, "}");
    okw(c, &d);
    jw_free(&d);
}

static void cmd_mode(const cmd *c, int follow)
{
    long n;
    char *m = str_of(c->obj, "mode", &n);
    int all = m && !strcmp(m, "all");
    if (m && !all && strcmp(m, "one-at-a-time"))
        fail(c, "Invalid mode");
    else {
        if (follow)
            c->r->s->follow_all = all;
        else
            c->r->s->steer_all = all;
        ok(c, 0);
    }
    free(m);
}

static void cmd_compact(const cmd *c)
{
    pi_rpc *r = c->r;
    long n;
    char *instr;
    if (busy(c))
        return;
    instr = str_of(c->obj, "customInstructions", &n);
    if (pi_store_compact(r->st, instr && instr[0] ? instr : 0)) {
        const char *e = r->st->err;
        if (!strncmp(e, "Compaction failed: ", 19))
            e += 19;
        fail(c, e);
    } else
        okw(c, &r->st->cp_result);
    free(instr);
}

static int flag_of(jv obj, const char *key, int def)
{
    jv v;
    if (json_get(obj, key, &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
        return json_type(v) == J_TRUE;
    return def;
}

static void cmd_bash(const cmd *c)
{
    pi_rpc *r = c->r;
    long n;
    char *line = str_of(c->obj, "command", &n);
    jw text, d;
    int code = 0, stopped = 0, excl = flag_of(c->obj, "excludeFromContext", 0);
    if (!line) {
        fail(c, "Missing command");
        return;
    }
    if (r->s->active) {
        fail(c, "Agent is already processing");
        free(line);
        return;
    }
    jw_init(&text);
    r->in_bash = 1;
    r->bash_abort = 0;
    if (pi_store_bash_run(r->st, line, excl, &text, &code, &stopped)) {
        fail(c, "The bash tool is not available");
    } else {
        jw_init(&d);
        if (text.n) {
            jw u;
            jw_init(&u);
            jw_rawz(&u, "{\"type\":\"bash_execution_update\"");
            if (c->id) {
                jw_rawz(&u, ",\"id\":");
                jw_raw(&u, c->id, c->idn);
            }
            jw_rawz(&u, ",\"delta\":");
            jw_str(&u, text.p, text.n);
            jw_rawz(&u, "}");
            put_line(r, &u);
            jw_free(&u);
        }
        jw_rawz(&d, "{\"output\":");
        jw_str(&d, text.p ? text.p : "", text.n);
        jw_rawz(&d, ",\"exitCode\":");
        jw_long(&d, code);
        jw_rawz(&d, stopped ? ",\"cancelled\":true" : ",\"cancelled\":false");
        jw_rawz(&d, ",\"truncated\":false}");
        okw(c, &d);
        jw_free(&d);
    }
    r->in_bash = 0;
    jw_free(&text);
    free(line);
}

static void cmd_export_html(const cmd *c)
{
    pi_rpc *r = c->r;
    long n;
    char *path = str_of(c->obj, "outputPath", &n);
    char done[700], err[200];
    cl_pitheme th;
    pitheme_system(&th);
    if (pi_export(r->st, &th, r->st->s3.cwd, path && path[0] ? path : 0, done, sizeof(done), err, sizeof(err)))
        fail(c, err);
    else {
        jw d;
        const char *p = done;
        if (!strncmp(p, "Session exported to: ", 21))
            p += 21;
        jw_init(&d);
        jw_rawz(&d, "{\"path\":");
        jw_strz(&d, path && path[0] ? path : p);
        jw_rawz(&d, "}");
        okw(c, &d);
        jw_free(&d);
    }
    free(path);
}

static void cmd_switch_session(const cmd *c)
{
    pi_rpc *r = c->r;
    long n;
    char *path;
    if (busy(c))
        return;
    path = str_of(c->obj, "sessionPath", &n);
    if (!path)
        fail(c, "Missing sessionPath");
    else {
        /* a relative path is the working directory's, as pi's process has it */
        char full[600];
        int rc;
        if (path[0] != '/' && !strchr(path, ':') && !path_join(r->st->cwd, path, full, sizeof(full)))
            ;
        else
            cl_copy(full, path, sizeof(full));
        rc = pi_store_switch(r->st, full, path);
        if (rc < 0)
            fail(c, r->st->err);
        else
            ok(c, rc ? "{\"cancelled\":true}" : "{\"cancelled\":false}");
    }
    free(path);
}

static void cmd_fork(const cmd *c)
{
    pi_rpc *r = c->r;
    pi_s3 *t = &r->st->s3;
    long n;
    char *id;
    int idx, rc;
    jw text, reply, d;
    if (busy(c))
        return;
    id = str_of(c->obj, "entryId", &n);
    idx = id ? s3_find(t, id) : -1;
    if (idx < 0 || t->e[idx].type != S3_MESSAGE || t->e[idx].role != S3R_USER) {
        /* runtime.fork emits session_before_fork before it looks at the entry */
        if (pi_ev_before_fork(r->s, id ? id : "", "before"))
            ok(c, "{\"cancelled\":true}");
        else
            fail(c, "Invalid entry ID for forking");
        free(id);
        return;
    }
    free(id);
    jw_init(&text);
    jw_init(&reply);
    pi_user_text(t, idx, &text);
    rc = pi_store_branch(r->st, t->e[idx].parent, s3_id(t, idx), "before", &reply);
    if (rc < 0)
        fail(c, reply.p && !strncmp(reply.p, "[ERROR] ", 8) ? reply.p + 8 : "Failed to fork");
    else if (rc)
        ok(c, "{\"cancelled\":true}");           /* result.selectedText is undefined: JSON.stringify leaves `text` out */
    else {
        jw_init(&d);
        jw_rawz(&d, "{\"text\":");
        jw_str(&d, text.p ? text.p : "", text.n);
        jw_rawz(&d, ",\"cancelled\":false}");
        okw(c, &d);
        jw_free(&d);
    }
    jw_free(&text);
    jw_free(&reply);
}

static void cmd_clone(const cmd *c)
{
    pi_rpc *r = c->r;
    jw reply;
    int rc;
    if (busy(c))
        return;
    if (r->st->s3.leaf < 0) {
        fail(c, "Cannot clone session: no current entry selected");
        return;
    }
    jw_init(&reply);
    rc = pi_store_branch(r->st, r->st->s3.leaf, s3_id(&r->st->s3, r->st->s3.leaf), "at", &reply);
    if (rc < 0)
        fail(c, reply.p && !strncmp(reply.p, "[ERROR] ", 8) ? reply.p + 8 : "Failed to clone");
    else
        ok(c, rc ? "{\"cancelled\":true}" : "{\"cancelled\":false}");
    jw_free(&reply);
}

static void cmd_get_fork_messages(const cmd *c)
{
    pi_s3 *t = &c->r->st->s3;
    int i, k = 0;
    jw d;
    jw_init(&d);
    jw_rawz(&d, "{\"messages\":[");
    /* pi's getUserMessagesForForking: every user message of the session, whatever branch it is on */
    for (i = 0; i < t->n; i++)
        if (t->e[i].type == S3_MESSAGE && t->e[i].role == S3R_USER) {
            jw tx;
            jw_init(&tx);
            pi_user_text(t, i, &tx);
            if (k++)
                jw_rawz(&d, ",");
            jw_rawz(&d, "{\"entryId\":");
            jw_strz(&d, s3_id(t, i));
            jw_rawz(&d, ",\"text\":");
            jw_str(&d, tx.p ? tx.p : "", tx.n);
            jw_rawz(&d, "}");
            jw_free(&tx);
        }
    jw_rawz(&d, "]}");
    okw(c, &d);
    jw_free(&d);
}

static void leaf_json(pi_s3 *t, jw *d)
{
    jw_rawz(d, ",\"leafId\":");
    if (t->leaf < 0)
        jw_rawz(d, "null");
    else
        jw_strz(d, s3_id(t, t->leaf));
}

static void cmd_get_entries(const cmd *c)
{
    pi_s3 *t = &c->r->st->s3;
    long n;
    char *since = str_of(c->obj, "since", &n);
    int i, from = 0, k = 0;
    jw d;
    if (since) {
        int at = s3_find(t, since);
        if (at < 0) {
            char err[200];
            cl_copy(err, "Entry not found: ", sizeof(err));
            cl_cat(err, since, sizeof(err));
            fail(c, err);
            free(since);
            return;
        }
        from = at + 1;
    }
    free(since);
    jw_init(&d);
    jw_rawz(&d, "{\"entries\":[");
    for (i = from; i < t->n; i++) {
        long ln;
        const char *x = s3_text(t, i, &ln);
        if (!x)
            continue;
        if (k++)
            jw_rawz(&d, ",");
        jw_raw(&d, x, ln);
    }
    jw_rawz(&d, "]");
    leaf_json(t, &d);
    jw_rawz(&d, "}");
    okw(c, &d);
    jw_free(&d);
}

static void cmd_get_tree(const cmd *c)
{
    pi_s3 *t = &c->r->st->s3;
    jw d;
    jw_init(&d);
    jw_rawz(&d, "{\"tree\":");
    if (tree_json(t, &d)) {
        fail(c, "out of memory");
        jw_free(&d);
        return;
    }
    leaf_json(t, &d);
    jw_rawz(&d, "}");
    okw(c, &d);
    jw_free(&d);
}

static void cmd_get_last_assistant_text(const cmd *c)
{
    jw tx, d;
    jw_init(&tx);
    jw_init(&d);
    pi_last_text(c->r->s, &tx);
    if (tx.n) {
        jw_rawz(&d, "{\"text\":");
        jw_str(&d, tx.p, tx.n);
        jw_rawz(&d, "}");
    } else
        jw_rawz(&d, "{}");
    okw(c, &d);
    jw_free(&tx);
    jw_free(&d);
}

static void cmd_set_session_name(const cmd *c)
{
    long n;
    char *nm = str_of(c->obj, "name", &n);
    char *p = nm;
    if (p) {
        long l = (long)strlen(p);
        while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t' || p[l - 1] == '\n' || p[l - 1] == '\r'))
            p[--l] = 0;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
    }
    if (!p || !*p)
        fail(c, "Session name cannot be empty");
    else if (pi_store_name(c->r->st, p))
        fail(c, "the name could not be recorded");
    else
        ok(c, 0);
    free(nm);
}

static void handle_cmd(const cmd *c)
{
    const char *t = c->type;
    if (!strcmp(t, "prompt"))
        cmd_prompt(c);
    else if (!strcmp(t, "steer"))
        cmd_queue(c, 0);
    else if (!strcmp(t, "follow_up"))
        cmd_queue(c, 1);
    else if (!strcmp(t, "abort"))
        cmd_abort(c);
    else if (!strcmp(t, "clear_queue"))
        cmd_clear_queue(c);
    else if (!strcmp(t, "new_session"))
        cmd_new_session(c);
    else if (!strcmp(t, "get_state"))
        cmd_get_state(c);
    else if (!strcmp(t, "get_messages"))
        cmd_get_messages(c);
    else if (!strcmp(t, "set_model"))
        cmd_set_model(c);
    else if (!strcmp(t, "cycle_model"))
        cmd_cycle_model(c);
    else if (!strcmp(t, "get_available_models"))
        cmd_get_available_models(c);
    else if (!strcmp(t, "set_thinking_level"))
        cmd_set_thinking_level(c);
    else if (!strcmp(t, "cycle_thinking_level"))
        cmd_cycle_thinking_level(c);
    else if (!strcmp(t, "get_available_thinking_levels"))
        cmd_get_available_thinking_levels(c);
    else if (!strcmp(t, "set_steering_mode"))
        cmd_mode(c, 0);
    else if (!strcmp(t, "set_follow_up_mode"))
        cmd_mode(c, 1);
    else if (!strcmp(t, "compact"))
        cmd_compact(c);
    else if (!strcmp(t, "set_auto_compaction")) {
        c->r->st->cp_enabled = flag_of(c->obj, "enabled", 1);
        ok(c, 0);
    } else if (!strcmp(t, "set_auto_retry")) {
        c->r->s->retry_on = flag_of(c->obj, "enabled", 1);
        ok(c, 0);
    } else if (!strcmp(t, "abort_retry")) {
        pi_rpc *r = c->r;
        if (!r->s->retry_waiting)           /* agent-session.ts abortRetry(): nothing to abort outside the wait */
            ok(c, 0);
        else {
            r->s->retry_stop = 1;
            r->retry_resp = 1;
            jw_reset(&r->retry_id);
            if (c->id)
                jw_raw(&r->retry_id, c->id, c->idn);
        }
    } else if (!strcmp(t, "bash"))
        cmd_bash(c);
    else if (!strcmp(t, "abort_bash")) {
        c->r->bash_abort = 1;
        ok(c, 0);
    } else if (!strcmp(t, "get_session_stats")) {
        jw d;
        jw_init(&d);
        stats_json(c->r, &d);
        okw(c, &d);
        jw_free(&d);
    } else if (!strcmp(t, "export_html"))
        cmd_export_html(c);
    else if (!strcmp(t, "switch_session"))
        cmd_switch_session(c);
    else if (!strcmp(t, "fork"))
        cmd_fork(c);
    else if (!strcmp(t, "clone"))
        cmd_clone(c);
    else if (!strcmp(t, "get_fork_messages"))
        cmd_get_fork_messages(c);
    else if (!strcmp(t, "get_entries"))
        cmd_get_entries(c);
    else if (!strcmp(t, "get_tree"))
        cmd_get_tree(c);
    else if (!strcmp(t, "get_last_assistant_text"))
        cmd_get_last_assistant_text(c);
    else if (!strcmp(t, "set_session_name"))
        cmd_set_session_name(c);
    else if (!strcmp(t, "get_commands"))
        cmd_get_commands(c);
    else {
        char err[200];
        cl_copy(err, "Unknown command: ", sizeof(err));
        cl_cat(err, t, sizeof(err));
        fail(c, err);
    }
}

static void handle_line(pi_rpc *r, const char *p, long n)
{
    jv obj, v;
    cmd c;
    char type[80];
    if (json_parse(p, n, &obj)) {
        respond(r, 0, 0, "parse", 0, 0, 0, "Failed to parse command: Invalid JSON");
        return;
    }
    if (json_type(obj) != J_OBJ) {
        /* pi reads command.type of whatever it parsed: nothing there is an unknown command */
        respond(r, 0, 0, 0, 0, 0, 0, "Unknown command: undefined");
        return;
    }
    type[0] = 0;
    if (json_get(obj, "type", &v) && json_type(v) == J_STR)
        json_str(v, type, sizeof(type));
    if (!strcmp(type, "extension_ui_response")) {
        char id[64];
        jv i;
        id[0] = 0;
        if (json_get(obj, "id", &i) && json_type(i) == J_STR)
            json_str(i, id, sizeof(id));
        if (r->ui_wait && !strcmp(id, r->ui_id)) {
            jw_reset(&r->ui_resp);
            jw_raw(&r->ui_resp, p, n);
            r->ui_got = 1;
        }
        return;
    }
    c.r = r;
    c.id = 0;
    c.idn = 0;
    if (json_get(obj, "id", &v)) {
        c.id = v.p;
        c.idn = v.n;
    }
    c.type = type[0] ? type : "undefined";
    c.obj = obj;
    if (!type[0]) {
        respond(r, c.id, c.idn, 0, 0, 0, 0, "Unknown command: undefined");
        return;
    }
    handle_cmd(&c);
}

/* a compaction in progress, for get_state */
static void track(void *u, const pi_event *e)
{
    pi_rpc *r = (pi_rpc *)u;
    if (e->type == PE_COMPACTION_START)
        r->compacting = 1;
    else if (e->type == PE_COMPACTION_END)
        r->compacting = 0;
    else if (e->type == PE_RETRY_END && r->retry_resp) {
        r->retry_resp = 0;
        respond(r, r->retry_id.n ? r->retry_id.p : 0, r->retry_id.n, "abort_retry", 1, 0, 0, 0);
    }
}

int pi_mode_rpc(pi_rt *rt)
{
    static pi_rpc r;
    pi_session *s = rt->s;
    jw line;
    int (*poll0)(void *u) = s->h.poll;
    int (*ask0)(void *u, int tool, const char *name, const char *what, int outside) = s->ask;
    void *ask0_u = s->ask_u;
    const pi_ext_ui *ui0 = s->ext.ui;
    memset(&r, 0, sizeof(r));
    r.rt = rt;
    r.s = s;
    r.st = rt->store;
    r.io = rt->io;
    r.in_cap = IN_CAP;
    r.in = (char *)malloc((size_t)r.in_cap);
    if (!r.in)
        return 1;
    r.poll = poll0;
    r.seed = (r.io->ms ? (unsigned long)r.io->ms(r.io->u) : 7UL) * 2654435761UL + 1UL;
    jw_init(&r.abort_id);
    jw_init(&r.retry_id);
    jw_init(&r.ui_resp);
    jw_init(&r.out);
    pi_jout_init(&r.jout, s, r.io);
    pi_subscribe(&s->em, pi_jout_event, &r.jout);
    pi_subscribe(&s->em, track, &r);
    s->h.poll = rpc_poll;
    s->ext.replace_binds = 2;           /* rpc-mode.ts: a replaced session is bound twice (finishSessionReplacement, then the command), so
                                         * the extensions' session_start comes twice -- pi 1.1.0's behaviour */
    s->ask = rpc_ask;
    s->ask_u = &r;
    rpc_ui.u = &r;
    s->ext.ui = &rpc_ui;                /* rpc-mode.ts:320 uiContext: ctx.hasUI is true and the dialogs go to the client */
    g_rpc = &r;
    jw_init(&line);
    for (;;) {
        int rc = next_line(&r, 1000, &line);
        if (rc < 0)
            break;
        if (rc == 1)
            handle_line(&r, line.p, line.n);
    }
    jw_free(&line);
    g_rpc = 0;
    s->h.poll = poll0;
    s->ext.replace_binds = 0;
    s->ask = ask0;
    s->ask_u = ask0_u;
    s->ext.ui = ui0;
    pi_unsubscribe(&s->em, pi_jout_event, &r.jout);
    pi_unsubscribe(&s->em, track, &r);
    pi_jout_free(&r.jout);
    jw_free(&r.abort_id);
    jw_free(&r.retry_id);
    jw_free(&r.ui_resp);
    jw_free(&r.out);
    free(r.in);
    return 0;
}
