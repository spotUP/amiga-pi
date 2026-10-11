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

/* the prompt for s's tools and the working directory, appended to out: 0, -1 */
int pi_system_prompt(const pi_session *s, const char *cwd, jw *out);

#endif
