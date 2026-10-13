/* see ext.h */
#include <stdlib.h>
#include <string.h>
#include "ext.h"
#include "claude/util.h"

void pi_exts_init(pi_exts *x)
{
    memset(x, 0, sizeof(*x));
}

int pi_ext_add(pi_exts *x, const pi_ext *e)
{
    int at = x->n, i;
    if (x->n == PI_EXTS)
        return -1;
    if (e->rank)
        for (i = 0; i < x->n; i++)
            if (x->e[i]->rank > e->rank) {
                at = i;
                break;
            }
    memmove(&x->e[at + 1], &x->e[at], sizeof(x->e[0]) * (size_t)(x->n - at));
    x->e[at] = e;
    x->n++;
    return 0;
}

int pi_exts_add_command(pi_exts *x, const pi_ext *owner, const char *name, const char *description, int handle)
{
    int i;
    for (i = 0; i < x->ncmd; i++)
        if (x->cmd[i].owner == owner && !strcmp(x->cmd[i].name, name))
            break;
    if (i == x->ncmd) {
        if (x->ncmd == PI_EXT_CMDS)
            return -1;
        x->ncmd++;
    }
    x->cmd[i].owner = owner;
    x->cmd[i].name = name;
    x->cmd[i].description = description;
    x->cmd[i].handle = handle;
    return 0;
}

int pi_exts_commands(const pi_exts *x, pi_ext_rcmd *out, int max)
{
    int i, j, k, n = 0;
    for (i = 0; i < x->n; i++)
        for (j = 0; j < x->ncmd; j++) {
            const pi_ext_cmd *c = &x->cmd[j];
            int total = 0, occurrence = 1, suffix, taken;
            char name[120], num[16];
            if (c->owner != x->e[i] || n == max)
                continue;
            for (k = 0; k < x->ncmd; k++)
                total += !strcmp(x->cmd[k].name, c->name);
            for (k = 0; k < n; k++)
                occurrence += !strcmp(out[k].c->name, c->name);
            strncpy(name, c->name, 80);
            name[80] = 0;
            if (total > 1) {
                cl_ltoa(occurrence, num);
                strcat(name, ":");
                strcat(name, num);
            }
            suffix = occurrence;
            do {
                taken = 0;
                for (k = 0; k < n; k++)
                    if (!strcmp(out[k].invocation, name))
                        taken = 1;
                if (taken) {
                    cl_ltoa(++suffix, num);
                    strncpy(name, c->name, 80);
                    name[80] = 0;
                    strcat(name, ":");
                    strcat(name, num);
                }
            } while (taken);
            out[n].c = c;
            strcpy(out[n].invocation, name);
            n++;
        }
    return n;
}

int pi_exts_run_command(pi_exts *x, const char *line)
{
    pi_ext_rcmd *r;
    const char *sp;
    char name[120];
    long n;
    int i, count, ran = 0;
    if (line[0] != '/' || !x->ncmd)
        return 0;
    sp = strchr(line, ' ');
    n = sp ? (long)(sp - line) - 1 : (long)strlen(line) - 1;
    if (n <= 0 || n >= (long)sizeof(name))
        return 0;
    memcpy(name, line + 1, (size_t)n);
    name[n] = 0;
    r = (pi_ext_rcmd *)malloc(sizeof(*r) * PI_EXT_CMDS);     /* 9 KB: not on a 68k task's stack */
    if (!r)
        return 0;
    count = pi_exts_commands(x, r, PI_EXT_CMDS);
    for (i = 0; i < count && !ran; i++)
        if (!strcmp(r[i].invocation, name)) {
            if (r[i].c->owner->command)
                r[i].c->owner->command(r[i].c->owner->u, r[i].c->handle, sp ? sp + 1 : "");
            ran = 1;
        }
    free(r);
    return ran;
}

void pi_exts_event(void *u, const pi_event *e)
{
    pi_exts *x = (pi_exts *)u;
    int i;
    if (e->type == PE_AGENT_START)
        x->turn = 0;
    for (i = 0; i < x->n; i++)
        if (x->e[i]->event)
            x->e[i]->event(x->e[i]->u, e);
    if (e->type == PE_TURN_END)
        x->turn++;
}

int pi_exts_tool_call(pi_exts *x, const char *id, const char *name, jw *args, char *reason, long cap)
{
    int i;
    x->calls++;
    x->terminate = 0;
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
    for (; times > 0; times--) {
        for (i = 0; i < x->n; i++)
            if (x->e[i]->session_start)
                x->e[i]->session_start(x->e[i]->u, reason, prev);
        if (x->bound)
            x->bound(x->bound_u, reason);
    }
}

int pi_exts_has(const pi_exts *x, const char *type)
{
    int i;
    for (i = 0; i < x->n; i++)
        if (x->e[i]->has && x->e[i]->has(x->e[i]->u, type))
            return 1;
    return 0;
}

int pi_exts_emit(pi_exts *x, const char *type, jw *payload, int flags, pi_ext_apply fn, void *fu)
{
    int i, stopped = 0;
    for (i = 0; i < x->n && !stopped; i++)
        if (x->e[i]->emit && x->e[i]->has && x->e[i]->has(x->e[i]->u, type)) {
            x->current = x->e[i];
            stopped = x->e[i]->emit(x->e[i]->u, type, payload, flags, fn, fu);
        }
    x->current = 0;
    return stopped;
}
