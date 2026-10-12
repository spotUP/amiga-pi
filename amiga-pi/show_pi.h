/* show_pi -- the transcript as pi's interactive mode writes it (plan N0): the start header, the user's
 * message, the answer's Markdown (view/md.c in its pi look), a tool call as pi's renderers draw it
 * (read, bash, write, edit, ls, find, grep: the header, the result as collapsed), errors. Every line goes
 * to the screen through tui_lines in a batch; the colours are the roles of a cl_pitheme (claude/theme.h).
 * The goldens are tests/pi/tui (pi 1.0.4, tools/pi_oracle/tui_gen.py). Portable C89. */
#ifndef PI_SHOW_H
#define PI_SHOW_H

#include "claude/tui.h"
#include "claude/theme.h"
#include "claude/ui.h"
#include "view/md.h"

typedef struct pi_show {
    cl_tui *t;
    const cl_pitheme *th;
    hl_theme hlt;
    md *md;
    vw_out vo;
    jw line;                    /* the line md is writing */
    jw batch;                   /* lines ready for the screen */
    int first;                  /* the answer has written nothing yet (it gets its blank line first) */
    int any;                    /* something is in the transcript: the next block starts a blank line below it */
    /* the tool call in progress */
    char name[24];
    jw args;
    unsigned long t0;
    long n_text, n_blocks, n_tools;
    int replay;                 /* tool results of an earlier run: no duration */
} pi_show;

void pshow_init(pi_show *s, cl_tui *t, const cl_pitheme *th);
void pshow_free(pi_show *s);
/* the colours changed (a theme chosen): the Markdown theme made again */
void pshow_theme(pi_show *s, const cl_pitheme *th);
/* the answer's renderer (cl_render) */
void pshow_render(pi_show *s, cl_render *r);
/* the start: the logo, what the program is, the keys */
void pshow_header(pi_show *s, const char *name, const char *version);
/* the user's message */
void pshow_user(pi_show *s, const char *text);
/* a line of the program's own, in a role's colour (PR_*), one blank line before it */
void pshow_note(pi_show *s, int role, const char *text);
/* an answer that ended in an error: pi shows the message in the error colour */
void pshow_error(pi_show *s, const char *text);
/* a tool call: its header is shown now, its result when it ends. args is the call's JSON object */
void pshow_tool_start(pi_show *s, const char *name, const char *args, long an);
void pshow_tool_end(pi_show *s, const char *result, long rn, int is_error);
/* a result's JSON blocks ([{"type":"text","text":...}]) as the text of their text blocks, joined by newlines */
long pshow_result_text(const char *json, long n, jw *out);

/* the conversation of ses drawn again from its messages (a branch left for another, a session resumed) */
struct pi_session;
void pshow_replay(pi_show *s, const struct pi_session *ses);

#endif
