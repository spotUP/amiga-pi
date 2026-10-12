/* piapi -- amiga-pi as a library (plan H6; pi's SDK, docs/sdk.md, createAgentSession): a C program links the agent in and
 * drives one session -- new, prompt, steer, follow_up, abort, subscribe -- with no mode in between. Static link: the
 * program's own main hands over the platform (app.h pi_platform: the console, the network, the clock, the machine) and the
 * command line it wants the session made from (--model, --session, --tools, ...), exactly as the shipped main does.
 * One session per process (the runtime pieces are static). Single threaded: prompt runs to the end on the caller's stack;
 * steer, follow_up and abort are called from a subscriber or the platform's hooks while it runs (a callback of pi_api_subscribe
 * sees every event of events.h).
 * Portable C89, host-tested (tests/test_pi_modes.c runs tests/pi/sdk_example.c). */
#ifndef PI_API_H
#define PI_API_H

#include "app.h"

typedef struct pi_api pi_api;

/* the session made from the command line (argv[0] is the program's name); 0 when it cannot be (a second session, bad
 * arguments: the message is on pl->io) */
pi_api *pi_api_new(const pi_platform *pl, int argc, char **argv);
/* the prompt to the end: the last answer's stop reason (LS_STOP, LS_ABORTED, LS_ERROR, ...). Retries and compaction as
 * in the programs; agent_settled is the last event */
int pi_api_prompt(pi_api *a, const char *text);
/* queue a message for the running prompt (pi's steer: after the turn's tool calls; follow_up: when the run would stop).
 * 0, -1 when the queue is full */
int pi_api_steer(pi_api *a, const char *text);
int pi_api_follow_up(pi_api *a, const char *text);
/* stop the running prompt at the next wait or tool call */
void pi_api_abort(pi_api *a);
/* a listener for the events (events.h); 0, -1 full */
int pi_api_subscribe(pi_api *a, pi_listener fn, void *u);
/* the text of the last answer (its text blocks joined) into out: its length */
long pi_api_text(pi_api *a, char *out, long cap);
/* the session itself, for what the API does not wrap (models, the store, tools) */
pi_session *pi_api_session(pi_api *a);
void pi_api_free(pi_api *a);

#endif
