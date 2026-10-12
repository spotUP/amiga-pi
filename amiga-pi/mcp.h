/* mcp -- amiga-pi's MCP client (plan phase K), after pi's built-in MCP
 * extension and its pi-mcp package (pi f10993bc
 * packages/coding-agent/src/extensions/mcp/{config,tools,index}.ts,
 * src/core/mcp-servers.ts, packages/mcp/src/{client,protocol/content}.ts,
 * src/extensions/tool-search/tool.ts, docs/mcp.md):
 *   config   mcp.json in pi's shape: ENVARC:amiga-pi/mcp.json, then
 *            <root>/.pi/mcp.json when the project is trusted (entries replace
 *            the user's; an entry without command/url/type overrides only
 *            enabled, exposure, toolExposure); pi's validation and words;
 *   client   JSON-RPC 2.0: initialize (protocol 2025-11-25, roots),
 *            notifications/initialized, tools/list with cursors, tools/call;
 *            the server's ping and roots/list answered, other requests
 *            refused (-32601);
 *   tools    mcp__<server>__<tool> (pi's sanitising and hash suffix), the
 *            server's schema, results as model content (text, images,
 *            resources as pi words them), text over 20 KB cut in the middle
 *            with the whole text kept in T: (pi_cut_path);
 *   exposure direct (declared), deferred (found and loaded by tool_search,
 *            BM25 as pi ranks), hidden; pi's codemode is not built: a
 *            codemode server is reached through tool_search, and a server
 *            without exposure is direct (COMPATIBILITY.md, MCP);
 *   section  pi's mcp_servers system prompt section for deferred servers.
 * Transports: mcp_http.c (streamable HTTP over llm_post: JSON and SSE
 * answers, Mcp-Session-Id, the LAN rule) and mcp_stdio.c (cl_sys spawn2;
 * PTY: pipes on the Amiga). Single-threaded: servers connect when the
 * session starts, one request at a time.
 * Portable C89 over cl_sys and llm/ (vtcon), host-tested (tests/test_pi_mcp.c). */
#ifndef PI_MCP_H
#define PI_MCP_H
#include "claude/json.h"
#include "claude/sys.h"
#include "llm/request.h"
#include "llm/creds.h"
#include "agent.h"

#define MCP_OUTPUT_MAX (20L * 1024)     /* pi's MCP_OUTPUT_MAX_BYTES */
#define MCP_NAME_MAX 64                 /* provider tool names */
#define MCP_TIMEOUT_MS 60000L           /* pi's default timeout (60 s) */
#define MCP_PROTOCOL "2025-11-25"       /* pi's LATEST_PROTOCOL_VERSION */
#define MCP_SEARCH_LIMIT 8              /* pi's DEFAULT_TOOL_SEARCH_LIMIT */

enum { MX_CODEMODE, MX_DEFERRED, MX_DIRECT, MX_HIDDEN, MX_NONE };
enum { MK_STDIO, MK_HTTP };

/* one incoming JSON-RPC message; 1 when it is the answer awaited */
typedef int (*mcp_msg_fn)(void *u, const char *msg, long n);

struct mcp;
typedef struct mcp_server mcp_server;

/* a transport: one message out, then (request 1) the messages in until on
 * returns 1 or the answer cannot come; 0, -1 with the server's err set */
typedef struct mcp_tr {
    void *u;
    int inline_reply;           /* a server's request is answered at once (stdio); else after the answer (HTTP) */
    int (*send)(mcp_server *s, const char *msg, long n, int request, mcp_msg_fn on, void *ou);
    void (*close)(mcp_server *s);
} mcp_tr;

typedef struct mcp_tool {
    pi_tool pt;                 /* the agent's tool: name, description, parameters, run */
    char *name;                 /* mcp__<server>__<tool> */
    char *tname;                /* the server's name for it */
    char *desc, *params;
    int exposure;               /* MX_* in effect: codemode counts as deferred */
    int active;                 /* declared to the model (direct, or loaded by tool_search) */
    mcp_server *srv;
} mcp_tool;

struct mcp_server {
    struct mcp *m;
    char name[64];
    char source[300];           /* the mcp.json that defined it */
    int kind;                   /* MK_* */
    int enabled;
    int exposure;               /* MX_*, MX_NONE when not configured */
    char *url, *headers;        /* http: the URL, headers as JSON object text (0 none) */
    char *command, *args, *env, *cwd;   /* stdio: args JSON array, env JSON object (0 none) */
    char *tool_exposure;        /* toolExposure as JSON object text, 0 none */
    char *description;
    long timeout_ms;
    /* the connection */
    int connected;
    char err[300];
    char session[160];          /* Mcp-Session-Id (http) */
    char version[24];           /* the protocol version the server chose */
    char *instructions;
    long next_id;
    mcp_tr tr;
    void *tu;                   /* the transport's state */
    jw pending;                 /* answers to the server's requests, sent after the current exchange (http) */
    mcp_tool *tools;
    int ntools;
    int expired_once;           /* a 404 for the session: initialized again once */
    int transient;              /* the last failure was a network error or 408, 429, 5xx but 501 (http) */
    int attempts;               /* connection attempts of the last connect (pi: 3 for http) */
};

typedef struct mcp {
    cl_sys *sys;
    llm_pool *pool;
    llm_hooks h;                /* poll, ms, sleep, resolve, notice */
    long (*getenv)(const char *name, char *out, long cap);
    const char *cwd, *tmp;
    mcp_server *srv;
    int n, cap;
    jw errors;                  /* one line each: what the config or a connection refused */
    pi_tool search;             /* tool_search, added when a deferred tool exists */
    int search_on;
    jw section;                 /* the mcp_servers section, empty none */
    pi_session *s;
    llm_cfg cfg;                /* the values of mcp.json (${VAR}, !command): the one resolver, llm/creds.c */
} mcp;

/* tools/call requests sent since the start (the reachability test's sentinel) */
extern long mcp_call_count;

const llm_hooks *mcp_hooks(mcp *m);
void mcp_init(mcp *m, cl_sys *sys, llm_pool *pool, const llm_hooks *h,
              long (*getenv)(const char *name, char *out, long cap), const char *cwd, const char *tmp);
void mcp_free(mcp *m);
/* an mcp.json: its servers added (project 1: the override rule); a missing
 * file is no error. Invalid entries go to errors and are skipped. 0, -1
 * (the file is not JSON of pi's shape; errors says so) */
int mcp_load(mcp *m, const char *path, int project);
int mcp_load_text(mcp *m, const char *text, long n, const char *path, int project);
/* every enabled server connected: initialize, tools/list; a failure in its
 * err and a line in errors. The number connected. */
int mcp_connect(mcp *m);
/* the tools to the session: direct ones declared, tool_search when a
 * deferred one exists, the mcp_servers section. 0, -1 too many tools */
int mcp_attach(mcp *m, pi_session *s);
/* an env or header value of server s as pi resolves it (resolveConfigValueOrThrow, no command cache): kind "env" or
 * "header", key its name. 0 with the value appended to out; -1 with pi's message in s->err ("Failed to resolve MCP
 * server "x" env "K" from environment variable: V"), the server then fails to connect */
int mcp_value(mcp_server *s, const char *kind, const char *key, const char *v, jw *out);
/* the server or tool by name, 0 none */
mcp_server *mcp_server_named(mcp *m, const char *name);
mcp_tool *mcp_tool_named(mcp *m, const char *name);

/* pi's createMcpToolName: mcp__<server>__<tool>, every character but
 * [A-Za-z0-9_] as '_'; over 64 or taken: cut and _<8 hex of sha256(server
 * NUL tool)> */
void mcp_tool_name(const char *server, const char *tool, int taken, char *out, long cap);
/* pi's limitMcpContent over model content blocks (a JSON array): text over
 * 20 KB as one block in Codex's format with the whole text written to
 * <tmp>amiga-pi-<id>.txt, images after it. 1 cut, 0 not */
int mcp_limit(cl_sys *sys, const char *tmp, const char *id, const char *blocks, long n, jw *out);
/* pi's convertMcpResult: a CallToolResult (JSON object) as model content
 * blocks (a JSON array, cut by mcp_limit): 0, -1 not a valid result (out
 * then holds pi's words for it). *is_error from the result. */
int mcp_result(mcp *m, const char *server, const char *tool, const char *id, const char *res, long n, jw *out,
               int *is_error);
/* the same, and what convertMcpResult returns beside the content: details ({server, tool, fullOutputPath when the
 * text was cut and saved}, a JSON object) and structuredContent (the CallToolResult without _meta, compact JSON), both
 * appended; left alone when the result is not valid */
int mcp_result_ex(mcp *m, const char *server, const char *tool, const char *id, const char *res, long n, jw *out,
                  int *is_error, jw *details, jw *structured);
/* tool_search: the deferred tools not loaded yet ranked for query (pi's
 * BM25), the best limit loaded into the session; the result text as pi
 * words it. 0, -1 (text holds the error) */
int mcp_search(mcp *m, pi_session *s, const char *query, long limit, jw *text);

/* the transports (mcp_http.c, mcp_stdio.c): 0, -1 err set */
int mcp_http_open(mcp_server *s);
int mcp_stdio_open(mcp_server *s);

#endif
