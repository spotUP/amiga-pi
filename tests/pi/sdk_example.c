/* sdk_example -- a program that uses amiga-pi as a library (piapi.h): one session, two prompts, a subscriber that counts
 * events and queues a steering message when the first tool call begins, the answer's text printed. Built into the test
 * binary (PI_TESTS) and run by tests/test_pi_modes.c with the platform of the test; a real program passes its own
 * (see main_posix.c). Returns 0 when the session answered both prompts. */
#include <stdio.h>
#include <string.h>
#include "../../amiga-pi/piapi.h"

typedef struct sdk_count {
    pi_api *api;
    long events, messages, tool_starts;
    int steered;
} sdk_count;

static void sdk_listener(void *u, const pi_event *e)
{
    sdk_count *c = (sdk_count *)u;
    c->events++;
    if (e->type == PE_MESSAGE_END)
        c->messages++;
    if (e->type == PE_TOOL_START) {
        c->tool_starts++;
        if (!c->steered) {
            c->steered = 1;
            pi_api_steer(c->api, "and keep it short");
        }
    }
}

/* counts: events, message_end events, tool starts */
int sdk_example(const pi_platform *pl, int argc, char **argv, char *answer, long cap, long counts[3])
{
    pi_api *api = pi_api_new(pl, argc, argv);
    sdk_count c;
    int rc1, rc2;
    if (!api)
        return 2;
    memset(&c, 0, sizeof(c));
    c.api = api;
    pi_api_subscribe(api, sdk_listener, &c);
    rc1 = pi_api_prompt(api, "read the file");
    rc2 = pi_api_prompt(api, "thanks");
    pi_api_text(api, answer, cap);
    pi_api_free(api);
    counts[0] = c.events;
    counts[1] = c.messages;
    counts[2] = c.tool_starts;
    return rc1 == LS_STOP && rc2 == LS_STOP ? 0 : 1;
}
