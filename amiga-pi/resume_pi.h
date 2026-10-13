/* resume_pi -- `amiga-pi -r` as pi 1.1.0 draws it: SessionSelectorComponent (modes/interactive/components/
 * session-selector.ts) over the sessions of the working directory, Tab for every directory, a search line typed
 * into, up/down, Enter, Escape. The frame is tui.h's tui_pick (session layout); the rows come out of the session
 * files (session3.h s3_infos), never from a second copy. Portable C89. */
#ifndef PI_RESUME_H
#define PI_RESUME_H

#include "claude/tui.h"
#include "llm/llm.h"

/* The selector on the console io, before any session is up (pi's startup screen). dir: the sessions of the working
 * directory; all: where Tab looks (a directory of directories, or with all_flat the one directory itself, as pi's
 * listAll with --session-dir); filter_cwd: keep the sessions whose header cwd is cwd (pi's list with a --session-dir
 * that is not the default); kb the text of keybindings.json (0: pi's defaults). Returns 1 with the session file in out, 0 cancelled (pi: "No session selected"), -1
 * when the console cannot show a screen. */
int pi_resume_pick(cl_io *io, cl_sys *sys, const cl_pitheme *th, llm_ts now, const char *cwd, const char *dir,
                   const char *all, int all_flat, int filter_cwd, const char *kb, long kbn, char *out, long cap);

/* pi's formatSessionDate: the age of `modified` (ms since 1970) at `now_ms` as "now", "5m", "3h", "2d", "1w", "4mo", "6y" */
void pi_resume_age(double modified, double now_ms, char *out, long cap);

#endif
