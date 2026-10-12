/* args -- amiga-pi's command line (plan C7): pi's flags that exist so far.
 *   amiga-pi [-p|--print] [--mode text|json|rpc] [--model provider/id] [--base-url URL]
 *            [--provider name] [--thinking level] [--api-key key] [--list-models [search]]
 *            [--tools|-t list] [--no-tools|-nt] [--gate|--no-gate] [--no-mcp]
 *            [--continue|-c] [--session path-or-id] [--resume|-r] [--no-session]
 *            [--name|-n name] [--approve|-a|--no-approve|-na] [prompt ...]
 * Project trust (resources.h): -a trusts this run's project .pi/ resources, -na does not, over every saved
 * decision and settings.json's defaultProjectTrust.
 * Sessions (store.h): -c continues the newest session of the working directory,
 * --session opens a file or the session whose id starts with the argument,
 * -r lists the sessions to pick one, --no-session keeps the conversation in RAM,
 * -n names the session (a session_info entry).
 * --tools: pi's allowlist of tools ("read,grep,find,ls"; "+ls" adds to the
 * defaults read, bash, edit, write); --no-tools: none. --gate / --no-gate
 * switch the permission gate (ext_gate.h) over settings.json's
 * permissionGate.
 * The model (--model takes pi's pattern: provider/id, an id, or part of one, with an optional :thinking level;
 * app.c resolves it in the registry of llm/registry.h). A provider the registry does not know is, with --base-url,
 * the old form: anthropic/<id> (anthropic-messages, api.anthropic.com),
 * openai/<id> (openai-completions, api.openai.com), any other provider is
 * openai-completions at --base-url (Ollama, LM Studio, a LAN server). No
 * --model: anthropic and C:Claude's default model. The key comes from
 * <PROVIDER>_API_KEY (ENV: on the Amiga), upper case, '-' as '_'.
 * Portable C89, host-tested. */
#ifndef PI_ARGS_H
#define PI_ARGS_H

#include "llm/backend.h"

#define PI_ARG_MSGS 16

enum { PI_MODE_TEXT, PI_MODE_JSON, PI_MODE_RPC };

typedef struct pi_args {
    int print;
    int mode;                   /* --mode: PI_MODE_* (pi's text, json, rpc) */
    const char *msg[PI_ARG_MSGS];   /* the messages, one prompt each (pi: [messages...]); pointers into argv */
    int nmsg;
    char tools[256];            /* --tools as given, "-" for --no-tools, "" the defaults */
    int gate;                   /* --gate 1, --no-gate 0, -1 settings.json decides */
    int cont, resume, no_session;
    int approve;                /* -a / --approve 1 trusts the project's .pi/ resources, -na / --no-approve 0, -1 ask */
    int no_mcp;                 /* --no-mcp: no MCP servers this run (pi's flag) */
    char session[300];          /* --session's argument, "" none */
    char name[128];             /* --name, "" none */
    char provider[48], id[96], base_url[256];   /* --model provider/id split as written (a provider the registry does not know) */
    char cli_provider[48];      /* --provider */
    char pattern[160];          /* --model as given: a pattern, optionally provider/ and :thinking */
    char api_key[512];          /* --api-key */
    int thinking;               /* --thinking as LT_*, -1 not given */
    int list_models;            /* --list-models */
    char list_search[64];
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
