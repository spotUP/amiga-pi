/* export -- /export (plan N4; pi interactive-mode handleExportCommand): the current branch of the session as a
 * standalone HTML page (the default, amiga-pi-session-<file>.html in the working directory) or, for a path ending
 * in .jsonl, as pi's serializeSessionBranch writes it. The page is static: pi's own is a script that renders the
 * session in the browser (marked, highlight.js: far too large for this machine); claude/html.c goes the other way
 * (HTML to Markdown) and does not apply. Portable C89. */
#ifndef PI_EXPORT_H
#define PI_EXPORT_H

#include "store.h"
#include "claude/theme.h"

/* arg: the command's argument, 0 none. 0 with "Session exported to: <path>" in done; -1 with pi's wording in err */
int pi_export(pi_store *st, const cl_pitheme *th, const char *cwd, const char *arg, char *done, long cap, char *err, long ecap);

#endif
