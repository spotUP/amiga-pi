/* see args.h */
#include <string.h>
#include "args.h"
#include "claude/util.h"
#include "claude/repl.h"        /* CL_DEFAULT_MODEL: C:Claude's default model */

int pi_args_parse(pi_args *a, int argc, char **argv)
{
    int i;
    memset(a, 0, sizeof(*a));
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "-p") || !strcmp(s, "--print"))
            a->print = 1;
        else if (!strcmp(s, "--model") || !strcmp(s, "--base-url")) {
            if (i + 1 >= argc) {
                cl_copy(a->err, s, sizeof(a->err));
                cl_cat(a->err, " needs a value", sizeof(a->err));
                return -1;
            }
            if (!strcmp(s, "--base-url"))
                cl_copy(a->base_url, argv[++i], sizeof(a->base_url));
            else {
                const char *v = argv[++i], *sl = strchr(v, '/');
                if (!sl || sl == v || !sl[1]) {
                    cl_copy(a->err, "--model takes provider/id, e.g. ollama/qwen2.5-coder:7b", sizeof(a->err));
                    return -1;
                }
                cl_copy(a->provider, v, sizeof(a->provider));
                a->provider[sl - v < (long)sizeof(a->provider) ? sl - v : (long)sizeof(a->provider) - 1] = 0;
                cl_copy(a->id, sl + 1, sizeof(a->id));
            }
        } else if (s[0] == '-' && s[1]) {
            cl_copy(a->err, "unknown option ", sizeof(a->err));
            cl_cat(a->err, s, sizeof(a->err));
            return -1;
        } else {
            if (a->prompt[0])
                cl_cat(a->prompt, " ", sizeof(a->prompt));
            cl_cat(a->prompt, s, sizeof(a->prompt));
        }
    }
    if (a->print && !a->prompt[0]) {
        cl_copy(a->err, "-p needs a prompt", sizeof(a->err));
        return -1;
    }
    return 0;
}

int pi_args_model(pi_args *a, llm_model *m)
{
    memset(m, 0, sizeof(*m));
    if (!a->provider[0]) {
        cl_copy(a->provider, "anthropic", sizeof(a->provider));
        cl_copy(a->id, CL_DEFAULT_MODEL, sizeof(a->id));
    }
    m->id = a->id;
    m->provider = a->provider;
    m->compat.max_tokens_field = m->compat.usage_in_streaming = m->compat.developer_role =
        m->compat.tool_result_name = LC_UNSET;
    if (!strcmp(a->provider, "anthropic")) {
        m->api = "anthropic-messages";
        m->base_url = a->base_url[0] ? a->base_url : "https://api.anthropic.com/v1";
        m->max_tokens = 8192;
    } else {
        m->api = "openai-completions";
        if (!strcmp(a->provider, "openai") && !a->base_url[0])
            m->base_url = "https://api.openai.com/v1";
        else if (a->base_url[0])
            m->base_url = a->base_url;
        else {
            cl_copy(a->err, "provider ", sizeof(a->err));
            cl_cat(a->err, a->provider, sizeof(a->err));
            cl_cat(a->err, " needs --base-url", sizeof(a->err));
            return -1;
        }
        m->max_tokens = 4096;
    }
    return 0;
}

void pi_key_name(const char *provider, char *out, long cap)
{
    long i, n = 0;
    for (i = 0; provider[i] && n < cap - 9; i++) {
        char c = provider[i];
        out[n++] = c == '-' ? '_' : c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    out[n] = 0;
    cl_cat(out, "_API_KEY", cap);
}
