/* see ext.h */
#include <string.h>
#include "ext.h"

void pi_exts_init(pi_exts *x)
{
    memset(x, 0, sizeof(*x));
}

int pi_ext_add(pi_exts *x, const pi_ext *e)
{
    if (x->n == PI_EXTS)
        return -1;
    x->e[x->n++] = e;
    return 0;
}

void pi_exts_event(void *u, const pi_event *e)
{
    pi_exts *x = (pi_exts *)u;
    int i;
    for (i = 0; i < x->n; i++)
        if (x->e[i]->event)
            x->e[i]->event(x->e[i]->u, e);
}

int pi_exts_tool_call(pi_exts *x, const char *id, const char *name, jw *args, char *reason, long cap)
{
    int i;
    x->calls++;
    for (i = 0; i < x->n; i++) {
        reason[0] = 0;
        if (x->e[i]->tool_call && x->e[i]->tool_call(x->e[i]->u, id, name, args, reason, cap)) {
            x->blocks++;
            return 1;
        }
    }
    reason[0] = 0;
    return 0;
}

void pi_exts_tool_result(pi_exts *x, const char *id, const char *name, const char *args, long an, jw *content,
                         int *is_error)
{
    int i;
    for (i = 0; i < x->n; i++)
        if (x->e[i]->tool_result)
            x->e[i]->tool_result(x->e[i]->u, id, name, args, an, content, is_error);
}

int pi_exts_input(pi_exts *x, jw *text)
{
    int i;
    for (i = 0; i < x->n; i++)
        if (x->e[i]->input && x->e[i]->input(x->e[i]->u, x->source ? x->source : "interactive", x->streaming, text))
            return 1;
    return 0;
}

void pi_exts_session_start(pi_exts *x, const char *reason, const char *prev)
{
    int i, times = strcmp(reason, "startup") && x->replace_binds > 1 ? x->replace_binds : 1;
    for (; times > 0; times--)
        for (i = 0; i < x->n; i++)
            if (x->e[i]->session_start)
                x->e[i]->session_start(x->e[i]->u, reason, prev);
}
