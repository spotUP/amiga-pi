#!/usr/bin/env python3
"""Writes tests/pi/sessions/*.jsonl: the entries of pi's docs/session-format.md
(f10993bc) as small sessions, plus the malformed and old-version files pi's tests
use.  The expected results are generated from pi's own code by gen.sh."""
import json, os, sys
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "pi", "sessions")
os.makedirs(out, exist_ok=True)
TS = "2024-12-03T14:00:%02d.000Z"
def hdr(**kw):
    h = {"type": "session", "version": 3, "id": "0193a1b2-c3d4-7e5f-8a6b-7c8d9e0f1a2b",
         "timestamp": TS % 0, "cwd": "/path/to/project"}
    h.update(kw)
    return h
def e(typ, i, pid, n, **kw):
    d = {"type": typ, "id": i, "parentId": pid, "timestamp": TS % n}
    d.update(kw)
    return d
USAGE = {"input": 10, "output": 5, "cacheRead": 0, "cacheWrite": 0, "totalTokens": 15,
         "cost": {"input": 0.1, "output": 0.2, "cacheRead": 0, "cacheWrite": 0, "total": 0.3}}
def user(t, text): return {"role": "user", "content": text, "timestamp": t}
def asst(t, text, model="claude-sonnet-4-5", stop="stop", extra=None):
    m = {"role": "assistant", "content": [{"type": "text", "text": text}], "api": "anthropic-messages",
         "provider": "anthropic", "model": model, "usage": USAGE, "stopReason": stop, "timestamp": t}
    if extra: m.update(extra)
    return m
def tres(t, cid, text): return {"role": "toolResult", "toolCallId": cid, "toolName": "bash",
                                  "content": [{"type": "text", "text": text}], "isError": False, "timestamp": t}
SYS = {"role": "system", "content": "", "sections": {"preamble": "You are an expert coding assistant.", "cwd": "/project"},
       "toolsAdded": [{"name": "read", "description": "Read a file", "parameters": {}}], "timestamp": 1733234400000}
def write(name, lines, raw=None):
    with open(os.path.join(out, name), "w") as f:
        if raw is not None:
            f.write(raw)
        else:
            for l in lines:
                f.write((l if isinstance(l, str) else json.dumps(l, separators=(",", ":"), ensure_ascii=False)) + "\n")

# 1. every entry type on one path (docs examples)
write("doc_all_types.jsonl", [
    hdr(),
    e("message", "a0b1c2d3", None, 0, message=SYS),
    e("model_change", "b1c2d3e4", "a0b1c2d3", 1, provider="anthropic", modelId="claude-sonnet-4-5"),
    e("thinking_level_change", "c2d3e4f5", "b1c2d3e4", 2, thinkingLevel="medium"),
    e("message", "a1b2c3d4", "c2d3e4f5", 3, message=user(1733234401000, "Hello")),
    e("message", "b2c3d4e5", "a1b2c3d4", 4, message=asst(1733234402000, "Hi!")),
    e("message", "c3d4e5f6", "b2c3d4e5", 5, message=tres(1733234403000, "call_123", "output")),
    e("model_change", "d4e5f6a7", "c3d4e5f6", 6, provider="openai", modelId="gpt-4o"),
    e("thinking_level_change", "e5f6a7b8", "d4e5f6a7", 7, thinkingLevel="high"),
    e("usage", "f6a7b8c9", "e5f6a7b8", 8, kind="cache_warm", provider="anthropic", model="claude-sonnet-4-5", usage=USAGE, note="warm"),
    e("custom", "h8i9a0b1", "f6a7b8c9", 9, customType="my-extension", data={"count": 42}),
    e("custom_message", "i9a0b1c2", "h8i9a0b1", 10, customType="my-extension", content="Injected context...", display=True),
    e("label", "a0b1c2d4", "i9a0b1c2", 11, targetId="a1b2c3d4", label="checkpoint-1"),
    e("session_info", "b1c2d3e5", "a0b1c2d4", 12, name="Refactor auth module"),
    e("message", "c2d3e4f6", "b1c2d3e5", 13, message=user(1733234413000, "Next")),
    e("message", "d3e4f5a6", "c2d3e4f6", 14, message=asst(1733234414000, "Done", model="gpt-4o")),
])

# 2. compaction (docs: the summary replaces the entries before firstKeptEntryId)
SYS2 = {"role": "system", "content": "You are a coding assistant.", "toolsAdded": [], "timestamp": 1733235000000}
write("doc_compaction.jsonl", [
    hdr(),
    e("message", "a0000001", None, 0, message=SYS),
    e("message", "a0000002", "a0000001", 1, message=user(1733234401000, "first question")),
    e("message", "a0000003", "a0000002", 2, message=asst(1733234402000, "first answer")),
    e("message", "a0000004", "a0000003", 3, message=user(1733234403000, "second question")),
    e("message", "a0000005", "a0000004", 4, message=asst(1733234404000, "second answer")),
    e("compaction", "a0000006", "a0000005", 5, summary="User discussed X, Y, Z...", firstKeptEntryId="a0000004",
      tokensBefore=50000, details={"readFiles": ["a.c"], "modifiedFiles": ["b.c"]}, usage=USAGE,
      systemMessage=SYS2),
    e("message", "a0000007", "a0000006", 6, message=user(1733234406000, "third question")),
    e("message", "a0000008", "a0000007", 7, message=asst(1733234407000, "third answer")),
    e("compaction", "a0000009", "a0000008", 8, summary="Older compactions are silent.", firstKeptEntryId="a0000007",
      tokensBefore=60000),
    e("message", "a000000a", "a0000009", 9, message=user(1733234409000, "fourth question")),
])
write("doc_compaction_retain_none.jsonl", [
    hdr(),
    e("message", "b0000001", None, 0, message=user(1733234401000, "q1")),
    e("message", "b0000002", "b0000001", 1, message=asst(1733234402000, "a1")),
    e("compaction", "b0000003", "b0000002", 2, summary="All of it.", firstKeptEntryId="b0000003", tokensBefore=100),
    e("message", "b0000004", "b0000003", 3, message=user(1733234404000, "q2")),
])

# 3. context_edit (docs: latest edit on the active branch wins; null omits; branch-relative)
write("doc_context_edit.jsonl", [
    hdr(),
    e("message", "c0000001", None, 0, message=user(1733234401000, "original user")),
    e("message", "c0000002", "c0000001", 1, message=asst(1733234402000, "original assistant")),
    e("message", "c0000003", "c0000002", 2, message=tres(1733234403000, "call_1", "original result")),
    e("custom_message", "c0000004", "c0000003", 3, customType="x", content="original custom", display=False),
    e("context_edit", "c0000005", "c0000004", 4, targetId="c0000001", replacement={"content": "first edit"}),
    e("context_edit", "c0000006", "c0000005", 5, targetId="c0000001", replacement={"content": "second edit"}),
    e("context_edit", "c0000007", "c0000006", 6, targetId="c0000002", replacement={"content": "assistant as string"}),
    e("context_edit", "c0000008", "c0000007", 7, targetId="c0000003", replacement=None),
    e("context_edit", "c0000009", "c0000008", 8, targetId="c0000004", replacement={"content": [{"type": "text", "text": "custom array"}]}),
    e("message", "c000000a", "c0000009", 9, message=user(1733234410000, "after the edits")),
])

# 4. tree with two branches and a branch summary (docs: Tree Structure)
write("doc_branch.jsonl", [
    hdr(),
    e("message", "d0000001", None, 0, message=user(1733234401000, "root question")),
    e("message", "d0000002", "d0000001", 1, message=asst(1733234402000, "root answer")),
    e("message", "d0000003", "d0000002", 2, message=user(1733234403000, "approach A")),
    e("message", "d0000004", "d0000003", 3, message=asst(1733234404000, "A answer")),
    e("branch_summary", "d0000005", "d0000002", 5, fromId="d0000004", summary="Branch explored approach A...",
      details={"readFiles": ["x.c"], "modifiedFiles": []}),
    e("message", "d0000006", "d0000005", 6, message=user(1733234406000, "approach B")),
    e("message", "d0000007", "d0000006", 7, message=asst(1733234407000, "B answer")),
    e("label", "d0000008", "d0000007", 8, targetId="d0000003", label="try-A"),
    e("label", "d0000009", "d0000008", 9, targetId="d0000003", label="try-A-again"),
    e("label", "d000000a", "d0000009", 10, targetId="d0000006", label="B"),
    e("label", "d000000b", "d000000a", 11, targetId="d0000006"),
])

# 5. unknown types, malformed lines, a custom type nobody knows, other roles
write("bad_lines.jsonl", [
    hdr(),
    e("message", "e0000001", None, 0, message=user(1733234401000, "kept")),
    "this is not json",
    "",
    '{"type":"future_thing","id":"e0000002","parentId":"e0000001","timestamp":"%s","x":1}' % (TS % 2),
    e("custom", "e0000003", "e0000002", 3, customType="unheard.of", data=[1, 2, 3]),
    e("message", "e0000004", "e0000003", 4, message={"role": "custom", "customType": "note", "content": "from an extension",
                                                     "display": True, "timestamp": 1733234404000}),
    e("message", "e0000005", "e0000004", 5, message={"role": "bashExecution", "command": "ls", "output": "a\nb\n",
                                                     "exitCode": 0, "cancelled": False, "truncated": False,
                                                     "timestamp": 1733234405000}),
    e("message", "e0000006", "e0000005", 6, message=asst(1733234406000, "ok")),
])
write("bad_no_final_newline.jsonl", None, raw=json.dumps(hdr(), separators=(",", ":")) + "\n" +
      json.dumps(e("message", "f0000001", None, 0, message=user(1733234401000, "no newline after me")), separators=(",", ":")))
write("bad_not_session.jsonl", [{"type": "message", "id": "11111111", "parentId": None, "timestamp": TS % 0,
                                  "message": user(1, "no header")}])

# 6. old versions (pi's migration cases)
write("mig_v1.jsonl", [
    {"type": "session", "id": "0193a1b2-v1v1-7e5f-8a6b-7c8d9e0f1a2b", "timestamp": TS % 0, "cwd": "/old"},
    {"type": "message", "timestamp": TS % 1, "message": user(1733234401000, "v1 user")},
    {"type": "message", "timestamp": TS % 2, "message": asst(1733234402000, "v1 answer")},
    {"type": "message", "timestamp": TS % 3, "message": user(1733234403000, "v1 second")},
    {"type": "compaction", "timestamp": TS % 4, "summary": "v1 summary", "firstKeptEntryIndex": 3, "tokensBefore": 500},
    {"type": "message", "timestamp": TS % 5, "message": user(1733234405000, "v1 last")},
])
write("mig_v2.jsonl", [
    hdr(version=2),
    e("message", "20000001", None, 0, message=user(1733234401000, "v2 user")),
    e("message", "20000002", "20000001", 1, message={"role": "hookMessage", "customType": "hook", "content": "from a hook",
                                                      "display": True, "timestamp": 1733234402000}),
    e("message", "20000003", "20000002", 2, message=asst(1733234403000, "v2 answer")),
])
# 7. parentSession header
write("doc_parent.jsonl", [
    hdr(parentSession="/path/to/original/session.jsonl"),
    e("message", "30000001", None, 0, message=user(1733234401000, "forked")),
])

# 8. the system message replay (getCurrentSystemMessage: content joined, sections patched by name,
# tools added and removed in order), a compaction carrying its own system message, null contents
T1 = {"name": "t1", "description": "one", "parameters": {"type": "object"}}
T2 = {"name": "t2", "description": "two", "parameters": {"type": "object"}}
T3 = {"name": "t3", "description": "three", "parameters": {"type": "object"}}
write("doc_system.jsonl", [
    hdr(),
    e("message", "50000001", None, 0, message={"role": "system", "content": "Base.", "sections": {"a": "A", "b": "B"},
                                              "toolsAdded": [T1, T2], "timestamp": 1733234400000}),
    e("message", "50000002", "50000001", 1, message=user(1733234401000, "q")),
    e("message", "50000003", "50000002", 2, message=asst(1733234402000, "a")),
    e("message", "50000004", "50000003", 3, message={"role": "system", "content": "More.", "sections": {"a": None, "c": "C"},
                                                    "toolsRemoved": [{"name": "t1"}], "toolsAdded": [T3],
                                                    "timestamp": 1733234403000}),
    e("message", "50000005", "50000004", 4, message=user(1733234404000, "q2")),
])
write("doc_compaction_system.jsonl", [
    hdr(),
    e("message", "60000001", None, 0, message=SYS),
    e("message", "60000002", "60000001", 1, message=user(1733234401000, "q1")),
    e("message", "60000003", "60000002", 2, message=asst(1733234402000, "a1")),
    e("message", "60000004", "60000003", 3, message=user(1733234403000, "q2")),
    e("compaction", "60000005", "60000004", 4, summary="Summary.", firstKeptEntryId="60000004", tokensBefore=10,
      systemMessage={"role": "system", "content": "Replayed.", "sections": {"x": "X"}, "timestamp": 1733234404000}),
    e("message", "60000006", "60000005", 5, message=user(1733234405000, "q3")),
])
write("bad_content.jsonl", [
    hdr(),
    e("message", "70000001", None, 0, message={"role": "system", "content": None, "timestamp": 1}),
    e("message", "70000002", "70000001", 1, message={"role": "user", "content": None, "timestamp": 2}),
    e("message", "70000003", "70000002", 2, message={"role": "assistant", "timestamp": 3}),
    e("message", "70000004", "70000003", 3, message={"role": "toolResult", "toolCallId": "c", "toolName": "t", "timestamp": 4}),
    e("custom_message", "70000005", "70000004", 4, customType="x", display=True),
    e("branch_summary", "70000006", "70000005", 5, fromId="70000001", summary=""),
    e("branch_summary", "70000007", "70000006", 6, fromId="70000001", summary="has text"),
    e("message", "70000008", "70000007", 7, message={"role": "bashExecution", "command": "ls", "output": "", "exitCode": 2,
                                                   "cancelled": False, "truncated": True, "fullOutputPath": "/tmp/x",
                                                   "timestamp": 8}),
    e("message", "70000009", "70000008", 8, message={"role": "bashExecution", "command": "echo", "output": "hi", "exitCode": None,
                                                   "cancelled": False, "truncated": False, "timestamp": 9,
                                                   "excludeFromContext": True}),
    e("message", "7000000a", "70000009", 9, message={"role": "bashExecution", "command": "sleep", "output": "o", "cancelled": True,
                                                   "truncated": False, "timestamp": 10}),
])
