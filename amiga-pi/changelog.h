/* changelog -- pi's utils/changelog.ts (pi 1.2.0): the entries of a CHANGELOG.md ("## [x.y.z] ..." and what follows), the ones newer
 * than a version, all of them oldest first (/changelog), and the version of the newest heading of a text. Portable C89. */
#ifndef PI_CHANGELOG_H
#define PI_CHANGELOG_H

#include "claude/json.h"

/* parseChangelog + getNewEntries: the entries of text newer than last (x.y.z; missing parts count 0), in the file's order, each
 * entry's lines trimmed, joined by a blank line into out. Returns how many entries */
int pi_cl_newer(const char *text, long n, const char *last, jw *out);

/* handleChangelogCommand: every entry, reversed (oldest first, as pi's .reverse() leaves them), joined by a blank line. Returns the count */
int pi_cl_all(const char *text, long n, jw *out);

/* the version of the first "## [x.y.z]" heading in md (interactive-mode.ts:858), "" when there is none */
void pi_cl_latest(const char *md, char *out, long cap);

#endif
