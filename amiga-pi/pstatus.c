/* see pstatus.h */
#include <string.h>
#include "pstatus.h"
#include "claude/util.h"
#include "claude/json.h"
#include "claude/tools_int.h"       /* tl_base64 */
#include "llm/llm.h"                /* LR_ASSISTANT */

#define MAX_MSG 2048

void pi_pstatus_format(jw *out, int state, const char *app, int kind, const char *msg)
{
    static const char *const names[] = { "idle", "working", "blocked", "done", "error", "clear" };
    static const char *const kinds[] = { "", "permission", "question", "auth" };
    char m[MAX_MSG + 8];
    long n = 0, i, end;
    int ok = 0;
    jw_rawz(out, "\033]7501;state=");
    jw_rawz(out, names[state]);
    if (app && *app && strlen(app) <= 32) {
        ok = 1;
        for (i = 0; app[i]; i++) {
            char c = app[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                  c == '+' || c == '-'))
                ok = 0;
        }
    }
    if (ok) {
        jw_rawz(out, ":app=");
        jw_rawz(out, app);
    }
    if (state == PST_BLOCKED && kind > PSK_NONE) {
        jw_rawz(out, ":kind=");
        jw_rawz(out, kinds[kind]);
    }
    /* control characters (runs) become one space, the text trimmed, cut to the limit at a character boundary */
    if (msg) {
        int ctl = 0;
        for (i = 0; msg[i] && n < MAX_MSG; i++) {
            unsigned char c = (unsigned char)msg[i];
            if (c < 0x20 || c == 0x7f) {
                ctl = 1;
                continue;
            }
            if (ctl)
                m[n++] = ' ';
            ctl = 0;
            m[n++] = (char)c;
        }
        if (n >= MAX_MSG && msg[i] && ((unsigned char)msg[i] & 0xC0) == 0x80) {
            /* cut inside a multi-byte character: drop it whole */
            while (n > 0 && ((unsigned char)m[n - 1] & 0xC0) == 0x80)
                n--;
            if (n > 0)
                n--;
        }
        end = n;
        while (end > 0 && m[end - 1] == ' ')
            end--;
        for (i = 0; i < end && m[i] == ' '; i++)
            ;
        if (end > i) {
            jw_rawz(out, ":msg=");
            tl_base64(out, (const unsigned char *)m + i, end - i);
        }
    }
    jw_rawz(out, "\033\\");
}

void pi_pstatus_init(pi_pstatus *p, int enabled, pi_pstatus_write write, void *u, const char *app,
                     const char *(*name)(void *u), void *name_u)
{
    memset(p, 0, sizeof(*p));
    p->enabled = enabled;
    p->write = write;
    p->u = u;
    p->app = app;
    p->name = name;
    p->name_u = name_u;
    p->result_state = PST_DONE;
    p->rest_state = PST_IDLE;
}

/* the first line of an error text, "Error" when there is none (firstLine) */
static void first_line(const char *text, long n, char *out, long cap)
{
    long i = 0, k = 0;
    if (!text)
        n = 0;
    while (i < n && text[i] != '\n' && text[i] != '\r' && k < cap - 1)
        out[k++] = text[i++];
    out[k] = 0;
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t'))
        out[--k] = 0;
    for (i = 0; out[i] == ' ' || out[i] == '\t'; i++)
        ;
    if (i)
        memmove(out, out + i, strlen(out + i) + 1);
    if (!out[0])
        cl_copy(out, "Error", cap);
}

static void report(pi_pstatus *p)
{
    jw o;
    int state, kind = PSK_NONE;
    const char *msg = 0;
    if (p->blocked) {
        state = PST_BLOCKED;
        kind = p->blocked_kind;
        msg = p->blocked_msg;
    } else if (p->compacting) {
        state = PST_WORKING;
        msg = "Compacting context";
    } else {
        state = p->run_active ? PST_WORKING : p->rest_state;
        if (state == PST_WORKING || state == PST_DONE)
            msg = p->name ? p->name(p->name_u) : 0;     /* the session's name; pi: only working and done carry it */
        else if (state == PST_ERROR)
            msg = p->rest_msg;
    }
    jw_init(&o);
    pi_pstatus_format(&o, state, p->app, kind, msg);
    if (o.p && strlen(o.p) < sizeof(p->last) && strcmp(o.p, p->last)) {
        cl_copy(p->last, o.p, sizeof(p->last));
        if (p->enabled && p->write) {
            p->write(p->u, o.p, o.n);
            p->reported = 1;
        }
    }
    jw_free(&o);
}

void pi_pstatus_event(pi_pstatus *p, const pi_event *e)
{
    switch (e->type) {
    case PE_AGENT_START:
        p->run_active = 1;
        p->result_state = PST_DONE;
        p->result_msg[0] = 0;
        break;
    case PE_MESSAGE_END:
        /* the latest response decides the outcome: a retried error is replaced by its successful retry */
        if (e->role != LR_ASSISTANT)
            return;
        {
            jv m, v;
            p->result_state = PST_DONE;
            p->result_msg[0] = 0;
            if (e->msg && !json_parse(e->msg, e->msg_n, &m) && json_get(m, "stopReason", &v) && json_streq(v, "error")) {
                char tx[300];
                tx[0] = 0;
                if (json_get(m, "errorMessage", &v) && json_type(v) == J_STR)
                    json_str(v, tx, sizeof(tx));
                p->result_state = PST_ERROR;
                first_line(tx, (long)strlen(tx), p->result_msg, sizeof(p->result_msg));
            }
        }
        break;
    case PE_COMPACTION_START:
        p->compacting = 1;
        break;
    case PE_COMPACTION_END:
        p->compacting = 0;
        if (p->run_active) {
            /* a failed recovery compaction ends the run unless a later response succeeds */
            if (e->aborted) {
                p->result_state = PST_IDLE;
            } else if (e->is_error) {
                p->result_state = PST_ERROR;
                first_line(e->msg, e->msg ? (long)strlen(e->msg) : 0, p->result_msg, sizeof(p->result_msg));
            }
        } else if (e->aborted) {
            p->rest_state = PST_IDLE;
        } else if (e->stop == 0) {          /* reason "manual" */
            if (e->is_error) {
                p->rest_state = PST_ERROR;
                first_line(e->msg, e->msg ? (long)strlen(e->msg) : 0, p->rest_msg, sizeof(p->rest_msg));
            } else
                p->rest_state = PST_DONE;
        }
        break;
    case PE_AGENT_SETTLED:
        p->run_active = 0;
        if (e->aborted) {
            p->rest_state = PST_IDLE;
        } else {
            p->rest_state = p->result_state;
            cl_copy(p->rest_msg, p->result_msg, sizeof(p->rest_msg));
        }
        break;
    case PE_SESSION_INFO:
        break;                              /* the name is part of the working and done reports */
    default:
        return;
    }
    report(p);
}

void pi_pstatus_blocked(pi_pstatus *p, int kind, const char *msg)
{
    p->blocked = kind != PSK_NONE;
    p->blocked_kind = kind;
    cl_copy(p->blocked_msg, msg ? msg : "", sizeof(p->blocked_msg));
    report(p);
}

void pi_pstatus_start(pi_pstatus *p)
{
    report(p);
}

void pi_pstatus_stop(pi_pstatus *p)
{
    jw o;
    if (!p->enabled || !p->write || !p->reported)
        return;
    jw_init(&o);
    pi_pstatus_format(&o, PST_CLEAR, 0, PSK_NONE, 0);
    p->write(p->u, o.p, o.n);
    jw_free(&o);
    p->reported = 0;
    p->last[0] = 0;
}
