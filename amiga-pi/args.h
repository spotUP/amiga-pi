/* args -- amiga-pi's command line (plan C7): pi's flags that exist so far.
 *   amiga-pi [-p|--print] [--model provider/id] [--base-url URL] [prompt ...]
 * The model: anthropic/<id> (anthropic-messages, api.anthropic.com),
 * openai/<id> (openai-completions, api.openai.com), any other provider is
 * openai-completions at --base-url (Ollama, LM Studio, a LAN server). No
 * --model: anthropic and C:Claude's default model. The key comes from
 * <PROVIDER>_API_KEY (ENV: on the Amiga), upper case, '-' as '_'.
 * Portable C89, host-tested. */
#ifndef PI_ARGS_H
#define PI_ARGS_H

#include "llm/backend.h"

typedef struct pi_args {
    int print;
    char provider[48], id[96], base_url[256];
    char prompt[4096];
    char err[160];
} pi_args;

/* 0, -1 with err set */
int pi_args_parse(pi_args *a, int argc, char **argv);
/* the model the arguments name (pointers into a): 0, -1 with a->err set */
int pi_args_model(pi_args *a, llm_model *m);
/* "OPENAI_API_KEY" for provider "openai" */
void pi_key_name(const char *provider, char *out, long cap);

#endif
