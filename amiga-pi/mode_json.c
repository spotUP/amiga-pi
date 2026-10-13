/* see mode_json.h */
#include <stdlib.h>
#include <string.h>
#include "mode_json.h"
#include "llm/accum.h"
#include "claude/util.h"

static void type_open(jw *w, const char *type)
{
    jw_rawz(w, "{\"type\":");
    jw_strz(w, type);
}

static void put_key(jw *w, const char *k)
{
    jw_rawz(w, ",");
    jw_strz(w, k);
    jw_rawz(w, ":");
}

static const llm_ablock *block_of(const llm_acc *a, long index)
{
    int i;
    for (i = a->nb - 1; i >= 0; i--)
        if (a->b[i].index == index)
            return &a->b[i];
    return 0;
}

/* the answer so far, as pi's message_start carries it: no content yet, the usage the stream reported with its cost,
 * stopReason pending (pi-ai's AssistantMessage while it streams) */
static void partial_message(const pi_session *s, const llm_acc *a, jw *w)
{
    llm_meta m;
    llm_ts ts;
    llm_acc_meta(a, &m);
    m.thinking_level = 0;       /* the agent loop adds it to the finished message only */
    ts.sec = 0;
    ts.ms = 0;
    if (s->now)
        ts = s->now(s->h.u);
    m.stop = LS_PENDING;
    llm_m_assistant(w, "[]", 2, &m, ts);
}

/* pi-ai's streaming parser keeps a tool call's arguments as far as the JSON text has come (partial-json): the text closed at
 * its end, an unfinished key or value left out; "{}" when nothing usable is there yet */
static void partial_args(const jw *a, jw *w)
{
    char *t, stack[64];
    long n = a->n, i, keep = 0;
    int depth = 0, in_str = 0, esc = 0, key_str = 0, expect_key = 0;
    jv v;
    if (!n) {
        jw_rawz(w, "{}");
        return;
    }
    if (json_parse(a->p, n, &v) == 0 && json_type(v) == J_OBJ) {
        jw_raw(w, a->p, n);
        return;
    }
    t = (char *)malloc((size_t)n + 80);
    if (!t) {
        jw_rawz(w, "{}");
        return;
    }
    /* keep: the length up to the last point where the text is a complete prefix that closing the open containers makes valid */
    for (i = 0; i < n; i++) {
        char c = a->p[i];
        if (in_str) {
            if (esc)
                esc = 0;
            else if (c == '\\')
                esc = 1;
            else if (c == '"') {
                in_str = 0;
                if (!key_str)
                    keep = i + 1;
            }
            continue;
        }
        if (c == '"') {
            in_str = 1;
            key_str = expect_key;
        } else if (c == '{' || c == '[') {
            if (depth < 64)
                stack[depth] = c;
            depth++;
            expect_key = c == '{';
            keep = i + 1;
        } else if (c == '}' || c == ']') {
            depth--;
            expect_key = 0;
            keep = i + 1;
        } else if (c == ',') {
            expect_key = depth > 0 && depth <= 64 && stack[depth - 1] == '{';
            keep = i;
        } else if (c == ':') {
            expect_key = 0;
        } else if ((c >= '0' && c <= '9') || c == 'e' || c == 'E' || c == '+' || c == '-' || c == '.' || c == 't' ||
                   c == 'r' || c == 'u' || c == 'f' || c == 'a' || c == 'l' || c == 's' || c == 'n') {
            /* a number or literal: complete only when something ends it (checked by the parse below) */
            long k = i;
            while (k < n && a->p[k] != ',' && a->p[k] != '}' && a->p[k] != ']' && a->p[k] != ' ')
                k++;
            if (k < n || (c >= '0' && c <= '9'))
                keep = k;
            i = k - 1;
        }
    }
    if (in_str && !key_str)
        keep = n;                               /* an open string value: closed where it stands */
    memcpy(t, a->p, (size_t)keep);
    t[keep] = 0;
    {
        /* the containers still open at keep, closed in reverse; a dangling key (`"a":`) or comma is cut */
        char open[64];
        int d = 0, st = 0, es = 0;
        long k, last_sig = 0;
        for (k = 0; k < keep; k++) {
            char c = t[k];
            if (st) {
                if (es)
                    es = 0;
                else if (c == '\\')
                    es = 1;
                else if (c == '"')
                    st = 0;
                continue;
            }
            if (c == '"')
                st = 1;
            else if (c == '{' || c == '[') {
                if (d < 64)
                    open[d] = c;
                d++;
            } else if (c == '}' || c == ']')
                d--;
        }
        k = keep;
        if (st) {
            t[k++] = '"';
            st = 0;
        }
        while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == ',' || t[k - 1] == ':'))
            k--;
        last_sig = k;
        /* a key with no value: `{"a":1,"b"` or `{"b":` -> drop back to the comma or brace before it */
        if (k > 0 && d > 0 && d <= 64 && open[d - 1] == '{') {
            long q = k - 1;
            if (t[q] == '"') {
                long r = q - 1;
                while (r >= 0 && !(t[r] == '"' && (r == 0 || t[r - 1] != '\\')))
                    r--;
                if (r >= 0) {
                    long b = r - 1;
                    while (b >= 0 && t[b] == ' ')
                        b--;
                    if (b >= 0 && (t[b] == '{' || t[b] == ','))
                        k = b + (t[b] == '{' ? 1 : 0);
                    last_sig = k;
                }
            }
        }
        k = last_sig;
        while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == ','))
            k--;
        while (d > 0) {
            d--;
            t[k++] = d < 64 && open[d] == '{' ? '}' : ']';
        }
        t[k] = 0;
        if (json_parse(t, k, &v) == 0 && json_type(v) == J_OBJ)
            jw_raw(w, t, k);
        else
            jw_rawz(w, "{}");
    }
    free(t);
}

/* the answer so far with the blocks so far (pi-ai's partial AssistantMessage, shared by message_update's `message` and every
 * `partial`): a block that has not ended carries its `index`, a tool call its raw `partialJson` and the arguments as far as
 * they parse */
void pi_partial_message(const pi_session *s, const llm_acc *a, jw *w)
{
    llm_meta m;
    llm_ts ts;
    jw c;
    int i;
    llm_acc_meta(a, &m);
    m.thinking_level = 0;
    m.stop = LS_PENDING;
    ts.sec = 0;
    ts.ms = 0;
    if (s->now)
        ts = s->now(s->h.u);
    jw_init(&c);
    jw_rawz(&c, "[");
    for (i = 0; i < a->nb; i++) {
        const llm_ablock *b = &a->b[i];
        if (i)
            jw_rawz(&c, ",");
        if (b->kind == LB_TOOLCALL) {
            jw_rawz(&c, "{\"type\":\"toolCall\",\"id\":");
            jw_strz(&c, b->id);
            jw_rawz(&c, ",\"name\":");
            jw_strz(&c, b->name);
            jw_rawz(&c, ",\"arguments\":");
            partial_args(&b->a, &c);
            if (!b->done) {
                jw_rawz(&c, ",\"partialJson\":");
                jw_str(&c, b->a.p ? b->a.p : "", b->a.n);
            }
        } else {
            jw_rawz(&c, b->kind == LB_THINKING ? "{\"type\":\"thinking\",\"thinking\":" : "{\"type\":\"text\",\"text\":");
            jw_str(&c, b->a.p ? b->a.p : "", b->a.n);
            if (b->sig.n) {
                jw_rawz(&c, b->kind == LB_THINKING ? ",\"thinkingSignature\":" : ",\"textSignature\":");
                jw_str(&c, b->sig.p, b->sig.n);
            }
        }
        if (!b->done) {
            jw_rawz(&c, ",\"index\":");
            jw_long(&c, b->index);
        }
        jw_rawz(&c, "}");
    }
    jw_rawz(&c, "]");
    llm_m_assistant(w, c.p, c.n, &m, ts);
    jw_free(&c);
}

/* pi's assistantMessageEvent for the stream event, appended to w; with_partial adds the cumulative `partial` (last). 1, or 0
 * for an event pi-ai does not model */
int pi_assistant_event(const pi_session *s, const pi_event *e, int with_partial, jw *w)
{
    const llm_event *a = e->ae;
    const llm_acc *acc = e->acc;
    const llm_ablock *b;
    const char *name = 0;
    if (!a || !acc)
        return 0;
    b = block_of(acc, a->index);
    if (a->type == LE_BLOCK_START) {
        if (a->kind == LB_TEXT)
            name = "text_start";
        else if (a->kind == LB_THINKING)
            name = "thinking_start";
        else if (a->kind == LB_TOOLCALL)
            name = "toolcall_start";
        else
            return 0;           /* a provider block pi-ai does not model */
    } else if (a->type == LE_DELTA) {
        if (!b || b->kind == LB_RAW)
            return 0;
        name = b->kind == LB_TEXT ? "text_delta" : b->kind == LB_THINKING ? "thinking_delta" : "toolcall_delta";
    } else if (a->type == LE_BLOCK_END) {
        if (!b || b->kind == LB_RAW)
            return 0;
        name = b->kind == LB_TEXT ? "text_end" : b->kind == LB_THINKING ? "thinking_end" : "toolcall_end";
    } else
        return 0;               /* a signature delta is part of the block, not an event */
    type_open(w, name);
    put_key(w, "contentIndex");
    jw_long(w, a->index);
    if (a->type == LE_BLOCK_START && a->kind == LB_TOOLCALL && !with_partial) {     /* the JSON record names the call; the extension's event does not */
        put_key(w, "id");
        jw_strz(w, b ? b->id : a->id ? a->id : "");
        put_key(w, "toolName");
        jw_strz(w, b ? b->name : a->name ? a->name : "");
    } else if (a->type == LE_DELTA) {
        put_key(w, "delta");
        jw_str(w, a->p ? a->p : "", a->p ? a->n : 0);
    } else if (a->type == LE_BLOCK_END) {
        if (b->kind == LB_TOOLCALL) {
            put_key(w, "toolCall");
            llm_b_toolcall(w, b->id, b->name, b->a.n ? b->a.p : "{}", b->a.n ? b->a.n : 2, 0);
        } else {
            put_key(w, "content");
            jw_str(w, b->a.p ? b->a.p : "", b->a.n);
        }
    }
    if (with_partial) {
        put_key(w, "partial");
        pi_partial_message(s, acc, w);
    }
    jw_rawz(w, "}");
    return 1;
}

/* message_update, JSON mode: pi's record has no cumulative message and no `partial` (usage in their place) */
static int update_event(const pi_session *s, const pi_event *e, jw *w)
{
    llm_meta m;
    jw ae;
    int ok;
    if (!e->ae || !e->acc)
        return 0;
    jw_init(&ae);
    ok = pi_assistant_event(s, e, 0, &ae);
    if (ok && !ae.oom) {
        llm_acc_meta(e->acc, &m);      /* the usage so far with its cost, as pi-ai's calculateCost keeps it on every update */
        jw_rawz(w, "{\"type\":\"message_update\",\"usage\":");
        llm_w_usage(w, &m.usage);
        jw_rawz(w, ",\"assistantMessageEvent\":");
        jw_raw(w, ae.p, ae.n);
        jw_rawz(w, "}");
    } else
        ok = 0;
    jw_free(&ae);
    return ok;
}

/* pi's Model object (types.ts Model): what get_state, set_model and the model events carry */
void pi_model_json(const llm_model *m, jw *w)
{
    jw_rawz(w, "{\"id\":");
    jw_strz(w, m->id);
    jw_rawz(w, ",\"name\":");
    jw_strz(w, m->name ? m->name : m->id);
    jw_rawz(w, ",\"api\":");
    jw_strz(w, m->api);
    jw_rawz(w, ",\"provider\":");
    jw_strz(w, m->provider);
    jw_rawz(w, ",\"baseUrl\":");
    jw_strz(w, m->base_url ? m->base_url : "");
    jw_rawz(w, m->reasoning ? ",\"reasoning\":true" : ",\"reasoning\":false");
    jw_rawz(w, m->image ? ",\"input\":[\"text\",\"image\"]" : ",\"input\":[\"text\"]");
    jw_rawz(w, ",\"cost\":{\"input\":");
    llm_w_micro(w, m->cost[0]);
    jw_rawz(w, ",\"output\":");
    llm_w_micro(w, m->cost[1]);
    jw_rawz(w, ",\"cacheRead\":");
    llm_w_micro(w, m->cost[2]);
    jw_rawz(w, ",\"cacheWrite\":");
    llm_w_micro(w, m->cost[3]);
    jw_rawz(w, "},\"contextWindow\":");
    jw_long(w, m->context_window);
    jw_rawz(w, ",\"maxTokens\":");
    jw_long(w, m->max_tokens);
    jw_rawz(w, "}");
}

static const char *reason_name(int r)
{
    return r == 0 ? "manual" : r == 1 ? "threshold" : "overflow";
}

int pi_json_event(const pi_session *s, const pi_event *e, jw *w)
{
    switch (e->type) {
    case PE_AGENT_START:
    case PE_TURN_START:
        type_open(w, pi_event_name(e->type));
        jw_rawz(w, "}");
        return 1;
    case PE_AGENT_SETTLED:                      /* agent-session.ts:1083-1085, 1.1.0: {type, aborted} */
        type_open(w, "agent_settled");
        put_key(w, "aborted");
        jw_rawz(w, e->aborted ? "true" : "false");
        jw_rawz(w, "}");
        return 1;
    case PE_MESSAGE_START:
    case PE_MESSAGE_END:
        type_open(w, pi_event_name(e->type));
        put_key(w, "message");
        if (e->msg)
            jw_raw(w, e->msg, e->msg_n);
        else if (e->acc)
            partial_message(s, e->acc, w);
        else
            return 0;
        jw_rawz(w, "}");
        return 1;
    case PE_MESSAGE_UPDATE:
        return update_event(s, e, w);
    case PE_TURN_END:
        type_open(w, "turn_end");
        put_key(w, "message");
        jw_raw(w, e->msg ? e->msg : "null", e->msg ? e->msg_n : 4);
        put_key(w, "toolResults");
        jw_raw(w, e->result ? e->result : "[]", e->result ? e->result_n : 2);
        jw_rawz(w, "}");
        return 1;
    case PE_AGENT_END:
        type_open(w, "agent_end");
        put_key(w, "messages");
        jw_raw(w, e->result ? e->result : "[]", e->result ? e->result_n : 2);
        put_key(w, "willRetry");
        jw_rawz(w, e->retry ? "true" : "false");
        jw_rawz(w, "}");
        return 1;
    case PE_TOOL_START:
        type_open(w, "tool_execution_start");
        put_key(w, "toolCallId");
        jw_strz(w, e->tool_id ? e->tool_id : "");
        put_key(w, "toolName");
        jw_strz(w, e->tool_name ? e->tool_name : "");
        put_key(w, "args");
        jw_raw(w, e->args ? e->args : "{}", e->args ? e->args_n : 2);
        jw_rawz(w, "}");
        return 1;
    case PE_TOOL_UPDATE:
        type_open(w, "tool_execution_update");
        put_key(w, "toolCallId");
        jw_strz(w, e->tool_id ? e->tool_id : "");
        put_key(w, "toolName");
        jw_strz(w, e->tool_name ? e->tool_name : "");
        put_key(w, "args");
        jw_raw(w, e->args ? e->args : "{}", e->args ? e->args_n : 2);
        put_key(w, "partialResult");
        jw_rawz(w, "{\"content\":");
        jw_raw(w, e->result ? e->result : "[]", e->result ? e->result_n : 2);
        if (e->structured) {
            jw_rawz(w, ",\"details\":");
            jw_raw(w, e->structured, e->structured_n);
        }
        jw_rawz(w, "}}");
        return 1;
    case PE_TOOL_END:
        type_open(w, "tool_execution_end");
        put_key(w, "toolCallId");
        jw_strz(w, e->tool_id ? e->tool_id : "");
        put_key(w, "toolName");
        jw_strz(w, e->tool_name ? e->tool_name : "");
        put_key(w, "result");
        jw_rawz(w, "{\"content\":");
        jw_raw(w, e->result ? e->result : "[]", e->result ? e->result_n : 2);
        if (e->details) {                       /* an MCP result: content, details, structuredContent, isError (convertMcpResult) */
            jw_rawz(w, ",\"details\":");
            jw_raw(w, e->details, e->details_n);
        }
        if (e->usage_early) {                   /* the tool's own: where its result literal put them */
            jw_rawz(w, ",\"usage\":");
            jw_raw(w, e->usage, e->usage_n);
        }
        if (e->usage_early && e->terminate)
            jw_rawz(w, ",\"terminate\":true");
        if (e->structured) {
            jw_rawz(w, ",\"structuredContent\":");
            jw_raw(w, e->structured, e->structured_n);
        }
        if ((e->details && e->is_error) || e->result_error)
            jw_rawz(w, ",\"isError\":true");
        if (e->thrown && !e->structured && !e->details)     /* a thrown error has details {}; bash's non-zero exit is a result */
            jw_rawz(w, ",\"details\":{}");
        if (e->usage && !e->usage_early) {      /* a handler's {usage} and a block's terminate come last (agent-loop.ts) */
            jw_rawz(w, ",\"usage\":");
            jw_raw(w, e->usage, e->usage_n);
        }
        if (e->terminate && !e->usage_early)
            jw_rawz(w, ",\"terminate\":true");
        jw_rawz(w, "}");
        put_key(w, "isError");
        jw_rawz(w, e->is_error ? "true" : "false");
        if (e->duration_ms >= 0) {              /* agent/src/agent-loop.ts emitToolExecutionEnd: after isError */
            put_key(w, "durationMs");
            jw_long(w, e->duration_ms);
        }
        jw_rawz(w, "}");
        return 1;
    case PE_COMPACTION_START:
        type_open(w, "compaction_start");
        put_key(w, "reason");
        jw_strz(w, reason_name(e->stop));
        jw_rawz(w, "}");
        return 1;
    case PE_COMPACTION_END:
        type_open(w, "compaction_end");
        put_key(w, "reason");
        jw_strz(w, reason_name(e->stop));
        if (e->result) {
            put_key(w, "result");
            jw_raw(w, e->result, e->result_n);
        }
        put_key(w, "aborted");
        jw_rawz(w, e->aborted ? "true" : "false");
        put_key(w, "willRetry");
        jw_rawz(w, e->retry ? "true" : "false");
        if (e->is_error && !e->aborted && e->msg) {
            put_key(w, "errorMessage");
            jw_str(w, e->msg, e->msg_n);
        }
        jw_rawz(w, "}");
        return 1;
    case PE_QUEUE_UPDATE:
        type_open(w, "queue_update");
        put_key(w, "steering");
        pi_queue_json(s, 0, w);
        put_key(w, "followUp");
        pi_queue_json(s, 1, w);
        jw_rawz(w, "}");
        return 1;
    case PE_THINKING_LEVEL:
        type_open(w, "thinking_level_changed");
        put_key(w, "level");
        jw_strz(w, e->msg ? e->msg : "");
        jw_rawz(w, "}");
        return 1;
    case PE_SESSION_INFO:
        type_open(w, "session_info_changed");
        if (e->msg) {
            put_key(w, "name");
            jw_str(w, e->msg, e->msg_n);
        }
        jw_rawz(w, "}");
        return 1;
    case PE_ENTRY_APPENDED:
        type_open(w, "entry_appended");
        put_key(w, "entry");
        jw_raw(w, e->msg ? e->msg : "null", e->msg ? e->msg_n : 4);
        jw_rawz(w, "}");
        return 1;
    case PE_RETRY_START:
        type_open(w, "auto_retry_start");
        put_key(w, "attempt");
        jw_long(w, e->n1);
        put_key(w, "maxAttempts");
        jw_long(w, e->n2);
        put_key(w, "delayMs");
        jw_long(w, e->n3);
        put_key(w, "errorMessage");
        jw_str(w, e->msg ? e->msg : "", e->msg ? e->msg_n : 0);
        jw_rawz(w, "}");
        return 1;
    case PE_RETRY_END:
        type_open(w, "auto_retry_end");
        put_key(w, "success");
        jw_rawz(w, e->retry ? "true" : "false");
        put_key(w, "attempt");
        jw_long(w, e->n1);
        if (!e->retry && e->msg) {
            put_key(w, "finalError");
            jw_str(w, e->msg, e->msg_n);
        }
        jw_rawz(w, "}");
        return 1;
    }
    return 0;
}

void pi_jout_init(pi_jout *j, pi_session *s, cl_io *io)
{
    memset(j, 0, sizeof(*j));
    j->s = s;
    j->io = io;
    jw_init(&j->line);
}

void pi_jout_free(pi_jout *j)
{
    jw_free(&j->line);
}

void pi_jout_line(cl_io *io, const char *p, long n)
{
    io->write(io->u, p, n);
    io->write(io->u, "\n", 1);
}

void pi_jout_event(void *u, const pi_event *e)
{
    pi_jout *j = (pi_jout *)u;
    jw_reset(&j->line);
    if (pi_json_event(j->s, e, &j->line) && !j->line.oom) {
        pi_jout_line(j->io, j->line.p, j->line.n);
        j->records++;
    }
}

int pi_mode_json(pi_session *s, pi_store *st, cl_io *io, const char *const *msgs, int n)
{
    pi_jout j;
    int i, rc = 0;
    pi_jout_init(&j, s, io);
    if (st && st->s3.head)
        pi_jout_line(io, st->s3.head, (long)strlen(st->s3.head));
    pi_subscribe(&s->em, pi_jout_event, &j);
    for (i = 0; i < n; i++)
        pi_prompt(s, msgs[i]);
    pi_unsubscribe(&s->em, pi_jout_event, &j);     /* the listener lives on this frame */
    pi_jout_free(&j);
    return rc;                  /* pi's JSON mode exits 0 whatever the last answer was: the records tell */
}
