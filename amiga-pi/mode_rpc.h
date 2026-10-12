/* mode_rpc -- pi's RPC mode (plan H2, H3, H5; pi dist/modes/rpc/rpc-mode.js, docs/rpc.md, rpc-commands.md,
 * rpc-extension-ui.md): the agent driven by JSON lines. Commands come as one JSON object per line on the console's input
 * (`{"id"?, "type", ...}`); the answer to each is `{"id"?, "type":"response", "command", "success", "data"|"error"}`;
 * the agent's events (mode_json.h, the one serializer) stream between them. All 33 commands of pi 1.0.4 (the `case` labels of
 * rpc-mode.js) are handled. Input ends: the program ends.
 *
 * The agent runs inside the `prompt` command's call, so a command that arrives meanwhile (steer, follow_up, abort,
 * get_state, ...) is read and handled from the platform's poll hook, which the agent calls on its waits and between tool
 * calls; the console must answer `read` (cl_io) with a timeout for that, else commands are read between runs only.
 * `abort` answers once the run has settled, as pi's does.
 *
 * Extension UI: an extension (ext.h) asks the user through pi_rpc_select / confirm / input / notify; the request is
 * written as `extension_ui_request` and the call returns when the client's `extension_ui_response` arrives (or its
 * timeout does). The permission gate's question (ext_gate.h) is a select.
 * Portable C89, host-tested (tests/test_pi_modes.c). */
#ifndef PI_MODE_RPC_H
#define PI_MODE_RPC_H

#include "app.h"

/* RPC mode to the end of the input: the exit code */
int pi_mode_rpc(pi_rt *rt);

/* ---- extension UI over RPC (valid while pi_mode_rpc runs; pi_rpc_ui() is 0 otherwise) ---- */
typedef struct pi_rpc pi_rpc;
pi_rpc *pi_rpc_ui(void);
/* the title and the options as pi's ctx.ui.select; timeout_ms 0: none. 1 with the chosen option in value, 0 cancelled or
 * timed out (pi: undefined) */
int pi_rpc_select(pi_rpc *r, const char *title, const char *const *options, int n, long timeout_ms, char *value, long cap);
/* 1 yes, 0 no, cancelled or timed out (pi: false) */
int pi_rpc_confirm(pi_rpc *r, const char *title, const char *message, long timeout_ms);
/* 1 with the text in value, 0 cancelled */
int pi_rpc_input(pi_rpc *r, const char *title, const char *placeholder, long timeout_ms, char *value, long cap);
/* fire and forget: type "info", "warning" or "error" (0: none given) */
void pi_rpc_notify(pi_rpc *r, const char *message, const char *type);

#endif
