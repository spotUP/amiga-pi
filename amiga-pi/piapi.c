/* see piapi.h */
#include <stdlib.h>
#include <string.h>
#include "piapi.h"

struct pi_api {
    pi_rt rt;
    int (*poll)(void *u);
    int abort_req;
    int open;
};

static pi_api api;

static int api_poll(void *u)
{
    int brk = api.poll ? api.poll(u) : 0;
    return brk || api.abort_req;
}

pi_api *pi_api_new(const pi_platform *pl, int argc, char **argv)
{
    if (api.open)
        return 0;
    memset(&api, 0, sizeof(api));
    if (pi_rt_open(&api.rt, pl, argc, argv))
        return 0;
    api.open = 1;
    api.poll = api.rt.s->h.poll;
    api.rt.s->h.poll = api_poll;
    return &api;
}

int pi_api_prompt(pi_api *a, const char *text)
{
    int rc;
    a->abort_req = 0;
    rc = pi_prompt(a->rt.s, text);
    a->abort_req = 0;
    return rc;
}

int pi_api_steer(pi_api *a, const char *text)
{
    return pi_steer(a->rt.s, text);
}

int pi_api_follow_up(pi_api *a, const char *text)
{
    return pi_follow_up(a->rt.s, text);
}

void pi_api_abort(pi_api *a)
{
    a->abort_req = 1;
}

int pi_api_subscribe(pi_api *a, pi_listener fn, void *u)
{
    return pi_subscribe(&a->rt.s->em, fn, u);
}

long pi_api_text(pi_api *a, char *out, long cap)
{
    jw t;
    long n;
    jw_init(&t);
    pi_last_text(a->rt.s, &t);
    n = t.n < cap - 1 ? t.n : cap - 1;
    if (cap > 0) {
        if (n > 0)
            memcpy(out, t.p, (size_t)n);
        out[n] = 0;
    }
    jw_free(&t);
    return n;
}

pi_session *pi_api_session(pi_api *a)
{
    return a->rt.s;
}

void pi_api_free(pi_api *a)
{
    if (!a->open)
        return;
    a->rt.s->h.poll = a->poll;
    pi_rt_close(&a->rt);
    a->open = 0;
}
