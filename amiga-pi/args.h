/* args -- amiga-pi's command line (plan C7): pi's flags that exist so far.
 *   amiga-pi [-p|--print] [--mode text|json|rpc] [--model provider/id] [--base-url URL]
 *            [--provider name] [--thinking level] [--api-key key] [--list-models [search]]
 *            [--tools|-t list] [--no-tools|-nt] [--no-builtin-tools|-nbt]
 *            [--exclude-tools|-xt list] [--gate|--no-gate] [--no-mcp]
 *            [--skill path] [--no-skills|-ns]
 *            [--continue|-c] [--session path-or-id] [--resume|-r] [--no-session]
 *            [--name|-n name] [--approve|-a|--no-approve|-na] [--version|-v] [prompt ...]
 * Project trust (resources.h): -a trusts this run's project .pi/ resources, -na does not, over every saved
 * decision and settings.json's defaultProjectTrust.
 * Sessions (store.h): -c continues the newest session of the working directory,
 * --session opens a file or the session whose id starts with the argument,
 * -r lists the sessions to pick one, --no-session keeps the conversation in RAM,
 * -n names the session (a session_info entry).
 * --tools: pi's allowlist of tools ("read,grep,find,ls") or, as a list of only +name/-name entries, changes to the
 * defaults read, bash, edit, write ("+ls", "-write"; mixing the two or a * in a modifier is an error, pi 1.1.0);
 * --no-tools: the defaults are empty and no extension or MCP tool joins; --no-builtin-tools: the defaults are empty,
 * extension and MCP tools stay; --exclude-tools: names and * patterns removed from every source, last (pi 1.1.0 cli/args.ts:147-165,
 * main.ts:540-550, sdk.ts:282-294). --gate / --no-gate
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
#define PI_ARG_SKILLS 16
#define PI_ARG_TMPLS 8

/* the pi release amiga-pi matches (tag v1.1.0): the start header and --version both print it */
#define PI_VERSION "1.1.0"

enum { PI_MODE_TEXT, PI_MODE_JSON, PI_MODE_RPC };

typedef struct pi_args {
    int print;
    int mode;                   /* --mode: PI_MODE_* (pi's text, json, rpc) */
    const char *msg[PI_ARG_MSGS];   /* the messages, one prompt each (pi: [messages...]); pointers into argv */
    int nmsg;
    char tools[256];            /* --tools as given, "" none given */
    int no_tools;               /* --no-tools: the default selection is empty (a --tools list still applies) */
    int no_builtin_tools;       /* --no-builtin-tools / -nbt: the default selection is empty, extension and MCP tools stay */
    char exclude[512];          /* --exclude-tools / -xt as given, "" none */
    int gate;                   /* --gate 1, --no-gate 0, -1 settings.json decides */
    int cont, resume, no_session;
    int approve;                /* -a / --approve 1 trusts the project's .pi/ resources, -na / --no-approve 0, -1 ask */
    int version;                /* --version / -v: print PI_VERSION and exit 0, over every later check (pi main.ts:632) */
    int no_mcp;                 /* --no-mcp: no MCP servers this run (pi's flag) */
    int no_extensions;          /* --no-extensions / -ne: the Lua extensions are not discovered (ext_lua.h) */
    int no_skills;              /* --no-skills / -ns: no skills but those of --skill (skills.h) */
    const char *skills[PI_ARG_SKILLS];  /* --skill <path>, repeatable: a skill file or a directory of them; pointers into argv */
    int nskills;
    int no_templates;           /* --no-prompt-templates / -np: only the --prompt-template paths */
    char tmpl_paths[PI_ARG_TMPLS][300];   /* --prompt-template <file or directory>, repeatable */
    int ntmpl_paths;
    char session[300];          /* --session's argument, "" none */
    char session_dir[300];      /* --session-dir: where the sessions are stored and looked up, "" the default */
    char name[128];             /* --name, "" none */
    char provider[48], id[96], base_url[256];   /* --model provider/id split as written (a provider the registry does not know) */
    char cli_provider[48];      /* --provider */
    char pattern[160];          /* --model as given: a pattern, optionally provider/ and :thinking */
    char api_key[512];          /* --api-key */
    int thinking;               /* --thinking as LT_*, -1 not given */
    int tui_mode;               /* --tui-mode: 1 fullscreen, 2 regular, 0 settings.json decides */
    int list_models;            /* --list-models */
    char list_search[64];
    char prompt[4096];
    char err[160];
    char warn[1024];            /* "Warning: <message>" lines, newline separated, "" none (pi parsed.diagnostics type warning) */
} pi_args;

/* 0, -1 with err set */
int pi_args_parse(pi_args *a, int argc, char **argv);
/* the model the arguments name (pointers into a): 0, -1 with a->err set */
int pi_args_model(pi_args *a, llm_model *m);
/* "OPENAI_API_KEY" for provider "openai" */
void pi_key_name(const char *provider, char *out, long cap);

#endif
