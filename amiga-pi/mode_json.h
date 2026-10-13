/* mode_json -- pi's JSON event stream (plan H1; pi dist/modes/json-event.js, print-mode.js, docs/json.md):
 * the one serializer of the agent's events (events.h) shared by `--mode json` and RPC mode (mode_rpc.c). A record is
 * one JSON object and a line feed. `--mode json` writes the session header first, then every event of the prompts
 * given on the command line, and ends after them.
 *
 * pi's records are the session events of agent-session.ts as pi 1.0.4 writes them, with message_update reduced to
 * {usage, assistantMessageEvent} (no cumulative `message`, no `partial`). The messages are amiga-pi's (llm/): they
 * carry the fields of pi f10993bc (COMPATIBILITY.md lists what pi 1.0.4 adds).
 * Portable C89, host-tested (tests/test_pi_modes.c). */
#ifndef PI_MODE_JSON_H
#define PI_MODE_JSON_H

#include "claude/ui.h"
#include "agent.h"
#include "store.h"

/* the event as pi's JSON record (no line feed) appended to w: 1, or 0 when pi writes no record for it */
int pi_json_event(const pi_session *s, const pi_event *e, jw *w);

/* pi's Model object as JSON appended to w (RPC get_state / set_model, the model_select event) */
void pi_model_json(const llm_model *m, jw *w);

/* a listener that writes every event as a record on io (pi_subscribe it); u is a pi_jout */
typedef struct pi_jout {
    pi_session *s;
    cl_io *io;
    jw line;
    long records;
} pi_jout;

void pi_jout_init(pi_jout *j, pi_session *s, cl_io *io);
void pi_jout_free(pi_jout *j);
void pi_jout_event(void *u, const pi_event *e);
/* a record from text: the JSON object text in w followed by the line feed, on io */
void pi_jout_line(cl_io *io, const char *p, long n);

/* `--mode json`: the session header, then each message as a prompt, in order. The exit code (1 when the last answer
 * failed or was stopped, as print mode) */
int pi_mode_json(pi_session *s, pi_store *st, cl_io *io, const char *const *msgs, int n);

#endif
