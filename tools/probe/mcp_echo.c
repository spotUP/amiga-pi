/* mcp_echo -- a stdio MCP server that runs on the Amiga (phase K5, rig step
 * 10): one JSON-RPC message per line on its input, one answer per line on
 * its output, until its input ends. initialize (the client's protocol
 * version back), ping, tools/list (one tool, echo {text}), tools/call echo
 * -> "echo: <text> (<n> requests)"; other requests -32601, notifications
 * ignored. Reads its input 4096 bytes at a time, as a buffered server does:
 * that is what PIPE: cannot serve and PTY: can (ARCHITECTURE.md, K1).
 * Native AmigaDOS (vbcc), vtcon's claude/json.c for the JSON.
 *   make build/amiga/mcp-echo    (in this repo; VTCON_DIR as for make amiga) */
#include <string.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "claude/json.h"

static long requests;

static void send(jw *w)
{
    jw_rawz(w, "\n");
    Write(Output(), w->p, w->n);
}

static void handle(const char *p, long n)
{
    jv v, id, m, params, x, args;
    jw w;
    if (json_parse(p, n, &v) || json_type(v) != J_OBJ || !json_get(v, "method", &m) || !json_get(v, "id", &id))
        return;                 /* a notification, an answer: nothing to say */
    requests++;
    jw_init(&w);
    jw_rawz(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    jw_raw(&w, id.p, id.n);
    if (json_streq(m, "initialize")) {
        jw_rawz(&w, ",\"result\":{\"protocolVersion\":");
        if (json_get(v, "params", &params) && json_get(params, "protocolVersion", &x) && json_type(x) == J_STR)
            jw_raw(&w, x.p, x.n);
        else
            jw_rawz(&w, "\"2025-11-25\"");
        jw_rawz(&w, ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"mcp_echo\",\"version\":\"1.0\"},"
                    "\"instructions\":\"An echo server running on the Amiga.\"}}");
    } else if (json_streq(m, "ping"))
        jw_rawz(&w, ",\"result\":{}}");
    else if (json_streq(m, "tools/list"))
        jw_rawz(&w, ",\"result\":{\"tools\":[{\"name\":\"echo\",\"description\":\"Echo the text back, from the Amiga.\","
                    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
                    "\"required\":[\"text\"]}}]}}");
    else if (json_streq(m, "tools/call") && json_get(v, "params", &params) && json_get(params, "name", &x) &&
             json_streq(x, "echo")) {
        jw t;
        char num[16];
        long k = requests, i = 15;
        char *s = json_get(params, "arguments", &args) && json_get(args, "text", &x) ? json_strdup(x, 0) : 0;
        num[i] = 0;
        do {
            num[--i] = (char)('0' + k % 10);
            k /= 10;
        } while (k && i > 0);
        jw_init(&t);
        jw_rawz(&t, "echo: ");
        jw_rawz(&t, s ? s : "");
        jw_rawz(&t, " (");
        jw_rawz(&t, num + i);
        jw_rawz(&t, " requests)");
        jw_rawz(&w, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
        jw_str(&w, t.p, t.n);
        jw_rawz(&w, "}]}}");
        jw_free(&t);
        free(s);
    } else
        jw_rawz(&w, ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
    send(&w);
    jw_free(&w);
}

int main(void)
{
    static char buf[4096];
    jw line;
    long n, i, st;
    jw_init(&line);
    for (;;) {
        n = Read(Input(), buf, sizeof(buf));
        if (n <= 0)
            break;
        jw_raw(&line, buf, n);
        for (st = i = 0; i < line.n; i++)
            if (line.p[i] == '\n') {
                handle(line.p + st, i - st);
                st = i + 1;
            }
        if (st) {
            memmove(line.p, line.p + st, (size_t)(line.n - st));
            line.n -= st;
        }
    }
    jw_free(&line);
    return 0;
}
