/* prompt -- amiga-pi's default system prompt (plan C4): pi's named
 * sections (pi f10993bc packages/coding-agent/src/core/system-prompt.ts
 * buildSystemPromptSections: preamble untagged, every other section in a
 * tag of its name, joined by a blank line as getSystemMessageText does)
 * with an "amiga" section before cwd. pi's "docs" section names pi's own
 * README and docs, which amiga-pi does not ship: left out (COMPATIBILITY.md);
 * the "amiga" section takes its place (there exactly when pi's docs would be).
 * Phase G (s->res, resources.h): a SYSTEM.md replaces the preamble and drops
 * tools, rules and amiga (pi's customPrompt); APPEND_SYSTEM.md is the
 * "addendum" section; the context files the "project_context" section (pi's
 * renderProjectContext), both before cwd. Phase I (skills.h): the "skills" section (pi's formatSkillsForPrompt) follows the
 * project context, when the session has the read tool or bash.
 * Portable C89. */
#ifndef PI_PROMPT_H
#define PI_PROMPT_H

#include "agent.h"

#define PI_PSECS 10

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
