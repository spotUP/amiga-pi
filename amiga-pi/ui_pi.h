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
#include "claude/theme.h"
#include "agent.h"

/* the directory the footer shows (the working directory as the program has it) */
void pi_ui_set_cwd(const char *cwd);
/* the directory name for pi's window title, as node's path.basename gives it: the last part after '/' or, on the
 * Amiga, after the volume's ':' (trailing '/' ignored); a volume root ("Ram Disk:") gives the volume's name */
void pi_dir_name(const char *path, char *out, int n);
/* settings.json's quietStartup: the screen starts without the header */
void pi_ui_set_quiet(int quiet);     /* 0 the start header and the loaded resources, 1 neither (quietStartup true), 2 the header only ("header") */
/* a startup warning, shown as pi's showWarning after the start screen ("Warning: " is put in front of text); copied, at most four */
void pi_ui_add_warning(const char *text);
/* settings.json's tuiMode / fullscreenExitOutput / fullscreenScrollbar (TM1): regular 1 is pi's "regular" mode (the
 * main screen, the editor follows the content), 0 its default "fullscreen" (the alternate screen); hint_only 1 is
 * fullscreenExitOutput "resume-hint"; scrollbar_always 1 is fullscreenScrollbar "always" */
void pi_ui_set_tui(int regular, int hint_only, int scrollbar_always);
/* settings.json's outputPad (pi 1.1.0): 1 (default) or 0, the horizontal padding of messages, tool boxes and `!` output */
void pi_ui_set_output_pad(int pad);
/* PI_PROGRAM_STATUS=1 (pi 1.1.0): write the OSC 7501 program status reports (pstatus.h). 0 (default): none -- pi also
 * writes them when the terminal answers its query, which amiga-pi does not send */
void pi_ui_set_program_status(int on);
/* the command that resumes the session, asked for when the screen is left ("" none: pi prints the line only for a
 * session that has a file) */
void pi_ui_set_resume(const char *(*fn)(void *u), void *u);
/* the colours (claude/theme.h) and the keybindings.json text (pi's shape, 0: pi's defaults) the screen starts with;
 * the theme is copied, the text only read while the screen starts */
void pi_ui_set_theme(const cl_pitheme *th);

/* the themes /settings can switch to: their names in pi's order (system first, then by name) into names[i], at most max, how many; and a
 * theme setting ("dark", "light/dark": a terminal that reports no appearance is dark) loaded into th, 0 or -1. The program owns both */
typedef struct pi_ui_themes {
    int (*list)(void *u, char (*names)[64], int max);
    int (*load)(void *u, const char *setting, cl_pitheme *th);
    void *u;
} pi_ui_themes;
void pi_ui_set_themes(const pi_ui_themes *src);
/* the terminal draws images (PI_IMAGE_PROTOCOL kitty or iterm2, as pi-tui reads it): /settings lists Show images and Image width */
void pi_ui_set_images(int on);
void pi_ui_set_keybindings(const char *json, long n);
/* What's New and /changelog (D1; pi interactive-mode.ts getChangelogForDisplay, handleChangelogCommand): file, the CHANGELOG.md text (0: none:
 * /changelog says "No changelog entries found."), news, the Markdown of the entries newer than the last start's (0: none; pi shows them
 * once, between two rules, after the loaded resources), collapsed (collapseChangelog: the one-line form). The texts stay valid until the run ends */
void pi_ui_set_changelog(const char *file, long file_n, const char *news, long news_n, int collapsed);

/* the slash commands the menu offers (claude/tui.h cl_cmd: "/name", the description); 0: amiga-pi's own */
void pi_ui_set_commands(const cl_cmd *cmds, int n);
/* the tests: the provider selector of /login shows these rows (pi's whole list, tests/pi/tui/login_rows.txt) instead of the
 * providers this build has; 0 rows: the build's own */
struct pi_login_row;
void pi_ui_set_login_rows(const struct pi_login_row *rows, int n, int show_type);
/* more rows after the built-in ones, by slot (0 prompt templates; the others for the other kinds of command, in pi's
 * order); the array stays valid until the run ends, 0 clears the slot */
void pi_ui_set_extra_commands(int slot, const cl_cmd *cmds, int n);
/* the slash menu's rows for the commands extensions registered (ext.h): malloc'ed rows and their text, freed by the caller; the count */
int pi_ui_ext_menu(const pi_exts *x, cl_cmd **menu, char **menu_text);
/* -p: 0 when the answer completed (stop), 1 otherwise (the error on io) */
int pi_ui_print(pi_session *s, cl_io *io, const char *prompt);
/* the conversation until /exit, /quit or the end of input; first: a prompt
 * to start with, 0 none. 0 */
int pi_ui_run(pi_session *s, cl_io *io, const char *first);
/* the reachability tests' sentinel: /model <text> refreshes of every catalog (findExactModelMatch) so far */
extern long pi_model_cmd_refreshes;
extern long pi_model_refresh_limit_ms;

#endif
