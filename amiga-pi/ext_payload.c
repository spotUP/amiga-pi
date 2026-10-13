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
    case PE_MESSAGE_UPDATE: return "message_update";
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
    if (e->type == PE_MESSAGE_UPDATE) {
        /* MessageUpdateEvent (types.ts:1053): the cumulative message, then the stream event with the same message as `partial` */
        jw ae;
        int ok;
        if (!e->acc)
            return 0;
        jw_init(&ae);
        ok = pi_assistant_event(s, e, 1, &ae) && !ae.oom;
        if (ok) {
            jw_rawz(b, "{\"type\":\"message_update\",\"message\":");
            pi_partial_message(s, e->acc, b);
            jw_rawz(b, ",\"assistantMessageEvent\":");
            jw_raw(b, ae.p, ae.n);
            jw_rawz(b, "}");
        }
        jw_free(&ae);
        return ok;
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

void pi_ext_payload_input(jw *b, const jw *text, const jw *images, const char *source, const char *streaming)
{
    jw_rawz(b, "{\"type\":\"input\",\"text\":");
    jw_str(b, text->p ? text->p : "", text->p ? text->n : 0);
    if (images && images->n > 2) {
        jw_rawz(b, ",\"images\":");
        jw_raw(b, images->p, images->n);
    }
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

void pi_ext_payload_ui_prompt(jw *b, int start, const char *kind, const char *title)
{
    jw_rawz(b, start ? "{\"type\":\"ui_prompt_start\"" : "{\"type\":\"ui_prompt_end\"");
    jw_rawz(b, ",\"reason\":\"ui_prompt\",\"kind\":");
    jw_strz(b, kind);
    if (title && title[0]) {
        jw_rawz(b, ",\"title\":");
        jw_strz(b, title);
    }
    jw_rawz(b, "}");
}

void pi_ext_payload_model_select(jw *b, const llm_model *m, const llm_model *previous, const char *source)
{
    jw_rawz(b, "{\"type\":\"model_select\",\"model\":");
    pi_model_json(m, b);
    if (previous) {
        jw_rawz(b, ",\"previousModel\":");
        pi_model_json(previous, b);
    }
    jw_rawz(b, ",\"source\":");
    jw_strz(b, source);
    jw_rawz(b, "}");
}

void pi_ext_payload_thinking(jw *b, const char *level, const char *previous)
{
    jw_rawz(b, "{\"type\":\"thinking_level_select\",\"level\":");
    jw_strz(b, level);
    jw_rawz(b, ",\"previousLevel\":");
    jw_strz(b, previous);
    jw_rawz(b, "}");
}

void pi_ext_payload_session_info(jw *b, const char *name)
{
    jw_rawz(b, "{\"type\":\"session_info_changed\"");
    if (name) {
        jw_rawz(b, ",\"name\":");
        jw_strz(b, name);
    }
    jw_rawz(b, "}");
}

void pi_ext_payload_shutdown(jw *b, const char *reason, const char *target)
{
    jw_rawz(b, "{\"type\":\"session_shutdown\",\"reason\":");
    jw_strz(b, reason);
    if (target && target[0]) {
        jw_rawz(b, ",\"targetSessionFile\":");
        jw_strz(b, target);
    }
    jw_rawz(b, "}");
}

void pi_ext_payload_before_switch(jw *b, const char *reason, const char *target)
{
    jw_rawz(b, "{\"type\":\"session_before_switch\",\"reason\":");
    jw_strz(b, reason);
    if (target && target[0]) {
        jw_rawz(b, ",\"targetSessionFile\":");
        jw_strz(b, target);
    }
    jw_rawz(b, "}");
}

void pi_ext_payload_before_fork(jw *b, const char *entry_id, const char *position)
{
    jw_rawz(b, "{\"type\":\"session_before_fork\",\"entryId\":");
    jw_strz(b, entry_id);
    jw_rawz(b, ",\"position\":");
    jw_strz(b, position);
    jw_rawz(b, "}");
}

void pi_ext_payload_resources(jw *b, const char *cwd, const char *reason)
{
    jw_rawz(b, "{\"type\":\"resources_discover\",\"cwd\":");
    jw_strz(b, cwd);
    jw_rawz(b, ",\"reason\":");
    jw_strz(b, reason);
    jw_rawz(b, "}");
}

void pi_ext_payload_user_bash(jw *b, const char *command, int exclude, const char *cwd)
{
    jw_rawz(b, "{\"type\":\"user_bash\",\"command\":");
    jw_strz(b, command);
    jw_rawz(b, exclude ? ",\"excludeFromContext\":true,\"cwd\":" : ",\"excludeFromContext\":false,\"cwd\":");
    jw_strz(b, cwd);
    jw_rawz(b, "}");
}

int pi_ext_decide_cancel(jv o)
{
    jv v;
    return o.p && json_get(o, "cancel", &v) && json_type(v) == J_TRUE;
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

int pi_ext_decide_input(jv o, jw *text, jw *images)
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
        /* runner.ts emitInput: currentImages = result.images ?? currentImages */
        if (images && json_get(o, "images", &v) && json_type(v) == J_ARR) {
            jw_reset(images);
            jw_raw(images, v.p, v.n);
        }
    }
    return 0;
}
