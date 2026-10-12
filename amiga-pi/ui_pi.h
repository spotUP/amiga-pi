/* ui_pi -- amiga-pi's prompt (plan C6) on C:Claude's screen code as it is
 * (claude/ui.h, tui.h, show.h): the answer's text as it streams, a line per
 * tool call and per failed result, Ctrl+C (and Esc in the screen) stops
 * the request or the tool. With a console that answers raw mode and its
 * size (UP-Term), C:Claude's screen: the transcript, the input box, the
 * spinner; else the line mode at the window's own prompt. Print mode (-p):
 * the prompt to the end, the last answer's text on stdout.
 * Portable C89 over cl_io; host-tested (tests/test_pi_agent.c). */
#ifndef PI_UI_H
#define PI_UI_H

#include "claude/ui.h"
#include "claude/tui.h"
#include "agent.h"

/* the directory the footer shows (the working directory as the program has it) */
void pi_ui_set_cwd(const char *cwd);
/* settings.json's quietStartup: the screen starts without the header */
void pi_ui_set_quiet(int quiet);
/* the slash commands the menu offers (claude/tui.h cl_cmd: "/name", the description); 0: amiga-pi's own */
void pi_ui_set_commands(const cl_cmd *cmds, int n);
/* -p: 0 when the answer completed (stop), 1 otherwise (the error on io) */
int pi_ui_print(pi_session *s, cl_io *io, const char *prompt);
/* the conversation until /exit, /quit or the end of input; first: a prompt
 * to start with, 0 none. 0 */
int pi_ui_run(pi_session *s, cl_io *io, const char *first);

#endif
