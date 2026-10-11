/* context -- what a session shows the model (plan E3, E3b; pi f10993bc
 * session-manager.ts buildSessionProjection / buildContextEntries /
 * sessionEntryToContextMessages / projectContextEntry, messages.ts convertToLlm,
 * ai/transcript.ts getCurrentSystemMessage, ai/text.ts getSystemMessageText).
 *
 * ctx_build walks the path from the root to a leaf: the latest compaction
 * replaces what came before its first kept entry (system messages of the
 * older range are not kept; an older compaction inside the kept range is
 * silent), context_edit entries are applied (the latest per target, null
 * omits, a string becomes one text block for assistant and toolResult),
 * entries become pi's AgentMessages (custom, branchSummary,
 * compactionSummary beside the pi-ai roles). ctx_to_llm is convertToLlm:
 * bashExecution, custom, branchSummary, compactionSummary become user
 * messages. The system message is replayed from the transcript: content
 * joined, sections patched by name, tools added and removed in order.
 * Portable C89, host-tested (tests/test_pi_session.c) against pi's own
 * output. */
#ifndef PI_CONTEXT_H
#define PI_CONTEXT_H

#include "session3.h"

typedef struct pi_ctx {
    char **msg;                 /* AgentMessages, each JSON text in its own allocation */
    long *msg_n;
    int n, cap;
    char thinking[32];          /* "off" until a thinking_level_change */
    char provider[64], model[128];
    int has_model;              /* a model_change or an assistant message on the path */
    int oom;
} pi_ctx;

void ctx_init(pi_ctx *c);
void ctx_free(pi_ctx *c);
/* the context of the branch ending at leaf (-1: empty). 0, -1 out of memory or unreadable */
int ctx_build(pi_s3 *st, int leaf, pi_ctx *c);
/* {"messages":[...],"thinkingLevel":..,"model":..} as pi's JSON.stringify of the context */
void ctx_json(const pi_ctx *c, jw *out);
/* convertToLlm: a JSON array of pi-ai messages (appended) */
int ctx_to_llm(const pi_ctx *c, jw *out);
/* convertToLlm of one AgentMessage: its pi-ai message appended to out, 1; 0 none (left out) */
int ctx_msg_to_llm(const char *json, long n, jw *out);
/* getCurrentSystemMessage over the context's system messages: the one message, 1; 0 none */
int ctx_system(const pi_ctx *c, jw *out);
/* getSystemMessageText of a system message (JSON): its text appended raw (not quoted) */
int ctx_system_text(const char *json, long n, jw *out);

#endif
