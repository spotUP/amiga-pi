/* compact -- context compaction (plan E4; pi f10993bc core/compaction/
 * compaction.ts and utils.ts, agent-session.ts compact / threshold check).
 *
 * cp_estimate is estimateTokens (characters / 4, rounded up, counted in
 * UTF-16 units as JavaScript counts them); cp_context_tokens is
 * estimateProjectedContextTokens (the last answer's reported usage plus an
 * estimate of what followed it, not trusted after a context_edit or a
 * compaction); cp_should is shouldCompact. cp_prepare is prepareCompaction:
 * the latest compaction's retained tail is the start, the cut keeps about
 * keepRecentTokens (never at a tool result; trailing metadata entries
 * joined; a cut inside a turn gives a turn prefix to summarize on its own),
 * the file lists come from the read, write and edit calls and the earlier
 * compaction's details. cp_compact is compact(): the history summary (the
 * update prompt with <previous-summary> when there was one), the turn prefix
 * summary merged after it, the file lists appended; the model is asked
 * through a callback so the same code runs in the tests on pi's own prompts.
 * Portable C89. */
#ifndef PI_COMPACT_H
#define PI_COMPACT_H

#include "context.h"

#define CP_RESERVE 16384L
#define CP_KEEP 20000L

typedef struct cp_strs {
    char **s;
    int n, cap;
} cp_strs;

typedef struct pi_prep {
    int first_kept;             /* tree index of the first entry kept */
    int split;                  /* the cut is inside a turn */
    long tokens_before;
    pi_ctx hist;                /* messagesToSummarize */
    pi_ctx prefix;              /* turnPrefixMessages */
    char *prev_summary;         /* the earlier compaction's summary, 0 none */
    cp_strs read, written, edited;
} pi_prep;

/* a reply to a summarization request */
typedef struct cp_reply {
    jw text;                    /* the answer's text blocks joined by a newline */
    llm_usage usage;
    int stop;                   /* LS_* */
    char error[200];            /* errorMessage */
    int tool_call;              /* the answer called a tool */
} cp_reply;

/* asks the model for one summary: prompt the user message's text, max_tokens its cap. 0 reply filled, -1 */
typedef int (*cp_complete)(void *u, const char *prompt, long max_tokens, cp_reply *r);

typedef struct cp_result {
    jw summary;                 /* with the file lists appended */
    llm_usage usage;
    long tokens_before;
    int first_kept;             /* tree index */
    jw details;                 /* {"readFiles":[..],"modifiedFiles":[..]} */
} cp_result;

long cp_estimate(const char *msg, long n);
/* the context's tokens as pi estimates them for a branch ending at leaf */
long cp_context_tokens(pi_s3 *st, int leaf, const pi_ctx *c);
int cp_should(long tokens, long window, long reserve);
/* 1 prepared, 0 nothing to compact (leaf is a compaction, or no history), -1 out of memory */
int cp_prepare(pi_s3 *st, int leaf, long keep_recent, pi_prep *p);
void cp_prep_free(pi_prep *p);
/* the summary: 0, -1 with err (pi's wording: "Summarization failed: ...") */
int cp_compact(const pi_prep *p, long reserve, long model_max_tokens, const char *instructions, cp_complete fn,
               void *u, cp_result *out, char *err, long errcap);
void cp_result_free(cp_result *r);
/* serializeConversation over AgentMessages (converted to LLM messages first), appended to out */
void cp_serialize(const pi_ctx *msgs, jw *out);

#endif
