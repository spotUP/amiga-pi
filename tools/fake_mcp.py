#!/usr/bin/env python3
"""fake_mcp.py -- an MCP server for amiga-pi's tests (plan phase K), stdio or
streamable HTTP, standard library only. Host only.

    fake_mcp.py [--toolset NAME] [--log FILE]                     stdio: one JSON message per line
    fake_mcp.py --http PORT [--bind ADDR] [--sse] [--expire N] ...  HTTP: POST /mcp; PORT 0 picks one

With --http the first line on stdout is "port <n>" once the server listens.
--sse answers requests as text/event-stream (chunked), else application/json.
--expire N: the session id stops working after N requests (404), as an
expired session does. --fail-first N [--fail-status S]: the first N requests
get status S (503), a server that is starting. --pages: tools/list in two pages (a cursor). --log FILE:
one JSON line per message received, with the HTTP headers that matter.

Tools (toolset "fake", the default):
  echo {text}         -> "echo: <text>"
  big {n}             -> n bytes of text in lines ("line 0001 ... ü\\n", the
                         UTF-8 in it on purpose), for the 20 KB cut
  fail, fail_empty, structured, struct_and_text, mixed, empty, no_content {}
                      -> the result of that name in tests/pi/mcp/results.json
                         (mixed: one block of every kind)
  boom {}             -> a JSON-RPC error -32000 "boom happened"
  ask_roots {}        -> asks the client roots/list (stdio) and answers with them
A toolset NAME other than "fake" is tests/pi/mcp/<NAME>.json: {"instructions",
"tools": [...]}, each tool answering "called <name>".
"""
import argparse
import base64
import http.server
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# fail, fail_empty, structured, mixed, ...: the results tests/pi/mcp/results.json holds (the oracle's input too)
with open(os.path.join(HERE, "..", "tests", "pi", "mcp", "results.json")) as _f:
    RESULTS = json.load(_f)

FAKE_TOOLS = [
    {"name": "echo", "description": "Echo the text back.",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string", "description": "What to echo"}},
                     "required": ["text"]}},
    {"name": "big", "description": "A long text.", "inputSchema": {"type": "object", "properties": {"n": {"type": "number"}}}},
    {"name": "fail", "description": "Fails.", "inputSchema": {"type": "object"}},
    {"name": "fail_empty", "inputSchema": {}},
    {"name": "struct_and_text", "inputSchema": {"type": "object"}},
    {"name": "empty", "inputSchema": {"type": "object"}},
    {"name": "no_content", "inputSchema": {"type": "object"}},
    {"name": "boom", "title": "Boom", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "structured", "description": "  Structured only.  \n", "inputSchema": {"type": "object"}},
    {"name": "mixed", "description": "Every kind of content.", "inputSchema": {"type": "object"}},
    {"name": "ask_roots", "description": "Asks for the roots.", "inputSchema": {"type": "object"}},
]


def big_text(n):
    out, i = [], 0
    total = 0
    while total < n:
        line = "line %04d of the long answer, with ü and € in it\n" % i
        out.append(line)
        total += len(line.encode("utf-8"))
        i += 1
    s = "".join(out).encode("utf-8")[:n]
    return s.decode("utf-8", "ignore")


class Server:
    def __init__(self, args):
        self.args = args
        self.instructions = "Fake server for amiga-pi tests.\nSecond line."
        self.tools = FAKE_TOOLS
        self.toolset = args.toolset
        if args.toolset != "fake":
            with open(os.path.join(HERE, "..", "tests", "pi", "mcp", args.toolset + ".json")) as f:
                d = json.load(f)
            self.tools = d["tools"]
            self.instructions = d.get("instructions")

    def log(self, msg, headers=None):
        if not self.args.log:
            return
        rec = {"msg": msg}
        if headers is not None:
            rec["headers"] = headers
        with open(self.args.log, "a") as f:
            f.write(json.dumps(rec) + "\n")

    def result(self, msg, ask=None):
        """the answer to a request (a dict), or None for a notification"""
        method, mid, params = msg.get("method"), msg.get("id"), msg.get("params") or {}
        if mid is None:
            return None
        if method == "initialize":
            r = {"protocolVersion": params.get("protocolVersion", "2025-11-25"),
                 "capabilities": {"tools": {"listChanged": True}},
                 "serverInfo": {"name": "fake_mcp", "version": "1.0"}}
            if self.instructions:
                r["instructions"] = self.instructions
            return {"jsonrpc": "2.0", "id": mid, "result": r}
        if method == "ping":
            return {"jsonrpc": "2.0", "id": mid, "result": {}}
        if method == "tools/list":
            if self.args.pages:
                half = len(self.tools) // 2
                if params.get("cursor") == "page2":
                    return {"jsonrpc": "2.0", "id": mid, "result": {"tools": self.tools[half:], "nextCursor": None}}
                return {"jsonrpc": "2.0", "id": mid, "result": {"tools": self.tools[:half], "nextCursor": "page2"}}
            return {"jsonrpc": "2.0", "id": mid, "result": {"tools": self.tools}}
        if method == "tools/call":
            return {"jsonrpc": "2.0", "id": mid, **self.call(params.get("name"), params.get("arguments") or {}, ask)}
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "Method not found: %s" % method}}

    def call(self, name, a, ask):
        if self.toolset != "fake":
            return {"result": {"content": [{"type": "text", "text": "called %s" % name}]}}
        if name == "echo":
            return {"result": {"content": [{"type": "text", "text": "echo: %s" % a.get("text", "")}]}}
        if name == "big":
            return {"result": {"content": [{"type": "text", "text": big_text(int(a.get("n", 30000)))}]}}
        if name in RESULTS:
            return {"result": RESULTS[name]}
        if name == "boom":
            return {"error": {"code": -32000, "message": "boom happened"}}
        if name == "ask_roots":
            roots = ask("roots/list") if ask else None
            return {"result": {"content": [{"type": "text", "text": "roots: %s" % json.dumps(roots)}]}}
        return {"error": {"code": -32602, "message": "Unknown tool: %s" % name}}


def run_stdio(srv):
    out = sys.stdout
    nxt = [100]

    def ask(method):
        nxt[0] += 1
        out.write(json.dumps({"jsonrpc": "2.0", "id": nxt[0], "method": method}) + "\n")
        out.flush()
        line = sys.stdin.readline()
        srv.log(json.loads(line))
        return json.loads(line).get("result")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        srv.log(msg)
        # a notification on the way, as servers send them (progress, logging)
        if msg.get("method") == "tools/call":
            out.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/message",
                                  "params": {"level": "info", "data": "calling"}}) + "\n")
        r = srv.result(msg, ask)
        if r is not None:
            out.write(json.dumps(r) + "\n")
            out.flush()


def run_http(srv, args):
    state = {"session": None, "count": 0, "gen": 0, "fails": 0}

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def do_POST(self):
            n = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(n)
            msg = json.loads(body)
            hdrs = {k.lower(): v for k, v in self.headers.items()
                    if k.lower() in ("mcp-session-id", "mcp-protocol-version", "accept", "content-type",
                                     "authorization", "x-token", "x-run")}
            hdrs["peer"] = self.client_address[0]
            srv.log(msg, hdrs)
            if state["fails"] < args.fail_first:
                state["fails"] += 1
                return self.plain(args.fail_status, "not now")
            sid = self.headers.get("Mcp-Session-Id")
            if msg.get("method") == "initialize":
                state["gen"] += 1
                state["session"] = "sess-%d" % state["gen"]
                state["count"] = 0
            elif sid != state["session"]:
                return self.plain(400 if sid is None else 404, "unknown session")
            else:
                state["count"] += 1
                if args.expire and state["count"] > args.expire:
                    state["session"] = None
                    return self.plain(404, "session expired")
            r = srv.result(msg)
            if r is None:
                self.send_response(202)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            data = json.dumps(r).encode()
            self.send_response(200)
            self.send_header("Mcp-Session-Id", state["session"])
            if args.sse:
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                events = []
                if msg.get("method") == "tools/call":
                    events.append(b"event: message\ndata: " + json.dumps(
                        {"jsonrpc": "2.0", "method": "notifications/progress",
                         "params": {"progressToken": 1, "progress": 1}}).encode() + b"\n\n")
                events.append(b"id: 1\nevent: message\ndata: " + data + b"\n\n")
                for ev in events:
                    # in two chunks, split inside the event
                    for part in (ev[:len(ev) // 2], ev[len(ev) // 2:]):
                        self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            else:
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        def plain(self, code, text):
            b = text.encode()
            self.send_response(code)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

    # the test that started it closes its input when done: the server ends then
    import threading
    def watch():
        sys.stdin.read()
        os._exit(0)
    threading.Thread(target=watch, daemon=True).start()
    httpd = http.server.ThreadingHTTPServer((args.bind, args.http), H)
    sys.stdout.write("port %d\n" % httpd.server_address[1])
    sys.stdout.flush()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--http", type=int)
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--sse", action="store_true")
    p.add_argument("--expire", type=int, default=0)
    p.add_argument("--pages", action="store_true")
    p.add_argument("--fail-first", type=int, default=0)
    p.add_argument("--fail-status", type=int, default=503)
    p.add_argument("--toolset", default="fake")
    p.add_argument("--log")
    args = p.parse_args()
    srv = Server(args)
    if args.http is not None:
        run_http(srv, args)
    else:
        run_stdio(srv)


if __name__ == "__main__":
    main()
