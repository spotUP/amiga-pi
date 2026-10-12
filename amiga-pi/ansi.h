/* ansi -- pi's handling of escape sequences in the output of a user command (`!cmd`, RPC bash), after pi 1.1.0
 * packages/coding-agent/src/utils/ansi.ts (stripAnsi, splitIncompleteAnsiSuffix) and core/bash-executor.ts:73-115:
 * the output is stripped of its colour codes and other sequences, an unfinished sequence at the end of a chunk is held
 * back for the next chunk (so no fragment such as a stray `m` is left), and binary garbage and carriage returns are
 * removed. Portable C89, host-tested (tests/test_pi_tools.c). */
#ifndef PI_ANSI_H
#define PI_ANSI_H

#include "claude/json.h"

#define PI_ANSI_MAX_PENDING 256     /* pi: MAX_PENDING_ANSI_LENGTH, a longer unfinished sequence is processed as it is */

/* the bytes of in[0..n) that are safe to strip now: the rest (in + result) is an unfinished escape sequence (an OSC
 * without its terminator, a CSI without its final byte, a lone ESC) of at most PI_ANSI_MAX_PENDING bytes, to be put
 * in front of the next chunk. splitIncompleteAnsiSuffix. */
long pi_ansi_split(const char *in, long n);
/* in[0..n) without its escape sequences (ESC or U+009B introduced CSI, OSC up to BEL, ESC \ or U+009C) appended to out.
 * stripAnsi. */
void pi_ansi_strip(const char *in, long n, jw *out);
/* what pi's appendText makes of a chunk: stripAnsi, then sanitizeBinaryOutput (C0 controls other than tab, newline
 * and carriage return, U+FFF9..U+FFFB) and every carriage return removed; appended to out */
void pi_ansi_clean(const char *in, long n, jw *out);

#endif
