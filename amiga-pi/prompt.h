/* prompt -- amiga-pi's default system prompt (plan C4): pi's named
 * sections (pi f10993bc packages/coding-agent/src/core/system-prompt.ts
 * buildSystemPromptSections: preamble untagged, every other section in a
 * tag of its name, joined by a blank line as getSystemMessageText does)
 * with an "amiga" section before cwd. pi's "docs" section names pi's own
 * README and docs, which amiga-pi does not ship: left out (COMPATIBILITY.md).
 * Project context, skills and APPEND_SYSTEM.md come in phases G and I.
 * Portable C89. */
#ifndef PI_PROMPT_H
#define PI_PROMPT_H

#include "agent.h"

#define PI_PSECS 8

/* pi's SystemPromptSections: the named sections in order (buildSystemPromptSections), each one's text
 * in text at off/len -- preamble untagged, every other in a tag of its name */
typedef struct pi_psec {
    jw text;
    char name[PI_PSECS][24];
    long off[PI_PSECS], len[PI_PSECS];
    int n;
} pi_psec;

/* the sections for s's tools and the working directory: 0, -1 out of memory */
int pi_prompt_sections(const pi_session *s, const char *cwd, pi_psec *out);
void pi_psec_free(pi_psec *p);

/* the prompt for s's tools and the working directory, appended to out: the sections joined by a blank
 * line (getSystemMessageText of a system message that is only sections). 0, -1 */
int pi_system_prompt(const pi_session *s, const char *cwd, jw *out);

#endif
