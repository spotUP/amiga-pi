/* see events.h */
#include <string.h>
#include "events.h"

static const char *const names[PE_N] = {
    "agent_start", "turn_start", "message_start", "message_update", "message_end",
    "tool_execution_start", "tool_execution_update", "tool_execution_end", "turn_end", "agent_end"
};

void pi_emitter_init(pi_emitter *em)
{
    memset(em, 0, sizeof(*em));
}

int pi_subscribe(pi_emitter *em, pi_listener fn, void *u)
{
    if (em->n == PI_SUBS)
        return -1;
    em->fn[em->n] = fn;
    em->u[em->n++] = u;
    return 0;
}

void pi_emit(pi_emitter *em, const pi_event *e)
{
    int i;
    if (e->type >= 0 && e->type < PE_N)
        em->count[e->type]++;
    for (i = 0; i < em->n; i++)
        em->fn[i](em->u[i], e);
}

void pi_event_init(pi_event *e, int t)
{
    memset(e, 0, sizeof(*e));
    e->type = t;
    e->role = -1;
}

const char *pi_event_name(int type)
{
    return type >= 0 && type < PE_N ? names[type] : "";
}
