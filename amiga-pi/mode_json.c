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

/* message_update: pi's assistantMessageEvent for the stream event, without the cumulative message or `partial` */
static int update_event(const pi_event *e, jw *w)
{
    const llm_event *a = e->ae;
    const llm_acc *acc = e->acc;
    const llm_ablock *b;
    const char *name = 0;
    llm_meta m;
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
    llm_acc_meta(acc, &m);      /* the usage so far with its cost, as pi-ai's calculateCost keeps it on every update */
    jw_rawz(w, "{\"type\":\"message_update\",\"usage\":");
    llm_w_usage(w, &m.usage);
    jw_rawz(w, ",\"assistantMessageEvent\":");
    type_open(w, name);
    put_key(w, "contentIndex");
    jw_long(w, a->index);
    if (a->type == LE_BLOCK_START && a->kind == LB_TOOLCALL) {
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
    jw_rawz(w, "}}");
    return 1;
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
        return update_event(e, w);
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
        if (e->structured) {
            jw_rawz(w, ",\"structuredContent\":");
            jw_raw(w, e->structured, e->structured_n);
        }
        if ((e->details && e->is_error) || e->result_error)
            jw_rawz(w, ",\"isError\":true");
        if (e->thrown && !e->structured && !e->details)     /* a thrown error has details {}; bash's non-zero exit is a result */
            jw_rawz(w, ",\"details\":{}");
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
