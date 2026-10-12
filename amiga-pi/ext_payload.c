/* see ext_payload.h */
#include <stdlib.h>
#include <string.h>
#include "ext_payload.h"
#include "mode_json.h"

const char *pi_ext_event_name(int t)
{
    switch (t) {
    case PE_AGENT_START: return "agent_start";
    case PE_TURN_START: return "turn_start";
    case PE_MESSAGE_START: return "message_start";
    case PE_MESSAGE_END: return "message_end";
    case PE_TOOL_START: return "tool_execution_start";
    case PE_TOOL_UPDATE: return "tool_execution_update";
    case PE_TOOL_END: return "tool_execution_end";
    case PE_AGENT_END: return "agent_end";
    case PE_AGENT_SETTLED: return "agent_settled";
    }
    return 0;
}

int pi_ext_payload_event(pi_session *s, const pi_event *e, jw *b)
{
    if (e->type == PE_TURN_START) {
        llm_ts t;
        t.sec = 0;
        t.ms = 0;
        if (s->now)
            t = s->now(s->h.u);
        jw_rawz(b, "{\"type\":\"turn_start\",\"turnIndex\":");
        jw_long(b, s->ext.turn);
        jw_rawz(b, ",\"timestamp\":");
        llm_w_ts(b, t);                         /* the 13-digit milliseconds, in the 32 bits of a long too */
        jw_rawz(b, "}");
        return 1;
    }
    if (e->type == PE_AGENT_END) {
        jw_rawz(b, "{\"type\":\"agent_end\",\"messages\":");
        jw_raw(b, e->result ? e->result : "[]", e->result ? e->result_n : 2);
        jw_rawz(b, "}");
        return 1;
    }
    return pi_json_event(s, e, b);
}

void pi_ext_payload_call(jw *b, const char *name, const char *id, const jw *args)
{
    jw_rawz(b, "{\"type\":\"tool_call\",\"toolName\":");
    jw_strz(b, name);
    jw_rawz(b, ",\"toolCallId\":");
    jw_strz(b, id);
    jw_rawz(b, ",\"input\":");
    jw_raw(b, args->p ? args->p : "{}", args->p ? args->n : 2);
    jw_rawz(b, "}");
}

void pi_ext_payload_result(jw *b, const pi_exts *x, const char *name, const char *id, const char *args, long an,
                           const jw *content, int is_error)
{
    int details = x->details && x->details->n, structured = x->structured && x->structured->n;
    jw_rawz(b, "{\"type\":\"tool_result\",\"toolName\":");
    jw_strz(b, name);
    jw_rawz(b, ",\"toolCallId\":");
    jw_strz(b, id);
    jw_rawz(b, ",\"input\":");
    jw_raw(b, args && an ? args : "{}", args && an ? an : 2);
    jw_rawz(b, ",\"content\":");
    jw_raw(b, content->p ? content->p : "[]", content->p ? content->n : 2);
    if (details) {
        jw_rawz(b, ",\"details\":");
        jw_raw(b, x->details->p, x->details->n);
    } else if (x->thrown && !structured)
        jw_rawz(b, ",\"details\":{}");          /* a thrown error's result has details {} (mode_json.c) */
    if (structured) {
        jw_rawz(b, ",\"structuredContent\":");
        jw_raw(b, x->structured->p, x->structured->n);
    }
    jw_rawz(b, is_error ? ",\"isError\":true}" : ",\"isError\":false}");
}

void pi_ext_payload_input(jw *b, const jw *text, const char *source, const char *streaming)
{
    jw_rawz(b, "{\"type\":\"input\",\"text\":");
    jw_str(b, text->p ? text->p : "", text->p ? text->n : 0);
    jw_rawz(b, ",\"source\":");
    jw_strz(b, source);
    if (streaming) {
        jw_rawz(b, ",\"streamingBehavior\":");
        jw_strz(b, streaming);
    }
    jw_rawz(b, "}");
}

void pi_ext_payload_start(jw *b, const char *reason, const char *prev)
{
    jw_rawz(b, "{\"type\":\"session_start\",\"reason\":");
    jw_strz(b, reason);
    if (prev) {
        jw_rawz(b, ",\"previousSessionFile\":");
        jw_strz(b, prev);
    }
    jw_rawz(b, "}");
}

int pi_ext_decide_call(jv o, jw *args, char *reason, long cap)
{
    jv x;
    if (json_get(o, "input", &x) && json_type(x) == J_OBJ) {
        jw_reset(args);
        jw_raw(args, x.p, x.n);
    }
    if (json_get(o, "block", &x) && json_type(x) == J_TRUE) {
        reason[0] = 0;
        if (json_get(o, "reason", &x) && json_type(x) == J_STR)
            json_str(x, reason, cap);
        return 1;
    }
    return 0;
}

void pi_ext_decide_result(jv o, pi_exts *x, jw *content, int *is_error)
{
    jv v;
    if (json_get(o, "content", &v) && json_type(v) == J_ARR) {
        jw_reset(content);
        jw_raw(content, v.p, v.n);
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
        *is_error = json_type(v) == J_TRUE;
}

int pi_ext_decide_input(jv o, jw *text)
{
    jv v;
    char *t;
    long tn;
    if (!json_get(o, "action", &v))
        return 0;
    if (json_streq(v, "handled"))
        return 1;
    if (json_streq(v, "transform") && json_get(o, "text", &v) && (t = json_strdup(v, &tn)) != 0) {
        jw_reset(text);
        jw_raw(text, t, tn);
        free(t);
    }
    return 0;
}
