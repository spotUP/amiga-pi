/* show_pi -- the transcript as pi's interactive mode writes it (plan N0): the start header, the user's
 * message, the answer's Markdown (view/md.c in its pi look), a tool call as pi's renderers draw it
 * (read, bash, write, edit, ls, find, grep: the header, the result as collapsed), errors. Every line goes
 * to the screen through tui_lines in a batch; the colours are the roles of a cl_pitheme (claude/theme.h).
 * The goldens are tests/pi/tui (pi 1.1.0, tools/pi_oracle/tui_gen.py). Portable C89. */
#ifndef PI_SHOW_H
#define PI_SHOW_H

#include "claude/tui.h"
#include "claude/theme.h"
#include "claude/ui.h"
#include "view/md.h"

#define PS_BOXES 32

/* a tool box that waits for its result */
typedef struct ps_box {
    char id[80], name[24];
    jw args;
    unsigned long t0;           /* the clock when the tool started */
    long off, len;              /* the box's text in pi_show.shadow */
    int elapsed;                /* a running bash call: pi's "Elapsed <time>" row, drawn again each second */
    long ticks;                 /* whole seconds the row has shown */
    jw partial;                 /* the output so far (tool_execution_update) */
} ps_box;

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
    long n_text, n_blocks, n_tools;
    int pad;                    /* outputPad: 1 (pi's default) or 0 */
    int expanded;               /* pi's toolOutputExpanded (ctrl+o): the summaries show their text; drawn from here on, redraw_chat redraws the rest */
    int replay;                 /* tool results of an earlier run: no duration */
    /* the tool boxes still waiting for their result (pi's ToolExecutionComponents in pendingTools), oldest first. The
     * transcript is lines already on the screen, so a box that is not the last thing there is drawn again from a copy
     * (shadow) of everything written since the oldest of them began: base is the transcript's line count at that
     * point. Tools may start and end in any order. */
    ps_box box[PS_BOXES];
    int nbox;
    jw shadow;
    long base;
} pi_show;

void pshow_init(pi_show *s, cl_tui *t, const cl_pitheme *th);
void pshow_free(pi_show *s);
/* settings.json's outputPad (0 or 1) */
void pshow_set_pad(pi_show *s, int pad);
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
void pshow_tool_start(pi_show *s, const char *id, const char *name, const char *args, long an);
/* tool_execution_update: the output so far of a running bash call, with its "Elapsed" row (pi renderers/bash.ts:107) */
void pshow_tool_update(pi_show *s, const char *id, const char *result, long rn);
/* the clock: a running bash call's Elapsed row changes once a second (pi: setInterval(invalidate, 1000), bash.ts:131) */
void pshow_tick(pi_show *s);
/* duration_ms: the result's recorded durationMs (bash's `Took`), -1 when none is recorded: the call's clock is used live and
 * nothing is shown on replay */
void pshow_tool_end(pi_show *s, const char *id, const char *result, long rn, int is_error, long duration_ms);
/* a result's JSON blocks ([{"type":"text","text":...}]) as the text of their text blocks, joined by newlines */
long pshow_result_text(const char *json, long n, jw *out);

/* a command of the user's (! and !!, pi's BashExecutionComponent): between two rules in the bash colour (faint when
 * the output stays out of the context), the command, the last twenty rows of the output, how it ended */
void pshow_bash(pi_show *s, const char *command, const char *output, int exit_code, int cancelled, int excluded);
/* pi 1.1.0's CompactionSummaryMessageComponent, BranchSummaryMessageComponent (collapsed, or with the summary text as Markdown when s->expanded: ctrl+o)
 * and CustomMessageComponent (display true; the text as Markdown): a Box over customMessageBg, outputPad wide */
void pshow_compaction_summary(pi_show *s, long tokens_before, const char *summary, long n);
void pshow_branch_summary(pi_show *s, const char *summary, long n);
void pshow_custom(pi_show *s, const char *type, const char *text, long n);
/* the conversation of ses drawn again from its messages (a branch left for another, a session resumed) */
struct pi_session;
void pshow_replay(pi_show *s, const struct pi_session *ses);
/* the same from AgentMessages (context.h: custom, compactionSummary and branchSummary still as pi keeps them; pi pushes
 * only the converted user messages to the model, ses->msg) */
void pshow_replay_msgs(pi_show *s, char *const *msg, const long *msg_n, int nmsg);

#endif
