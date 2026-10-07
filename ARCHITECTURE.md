# amiga-pi architecture (milestone 0)

Plan: `~/Code/vtcon/thoughts/shared/plans/2026-10-07-amiga-pi.md` (phase A). Research: `~/Code/vtcon/thoughts/shared/research/2026-10-07_pi-like-amiga-agent.md` (R-numbers below). pi source facts: `thoughts/shared/research/2026-10-07_pi-sources-addendum.md` (pi commit f10993bc7f28145df1375f3ff39c7f5c4cfc05f0). Nothing here ran on an Amiga.

## Decisions recorded (owner, 2026-10-07)

| Id | Decision | Source |
|---|---|---|
| D1 | amiga-pi lives in its own LOCAL repo `~/Code/amiga-pi` (no remote, no push). | owner, this task |
| D2 | It shares C:Claude's core by REFERENCE: sources are compiled in place from `$(VTCON_DIR)` (default `~/Code/vtcon`), never copied (plan B5 reuse rule). | owner |
| D3 | The shell tool is named `bash` and runs `vsh -c` (the kit's bash-level shell); AmigaDOS Shell only as fallback when vsh is absent, and the model is told which. | owner, round 2 item 5 |
| D4 | Extension language: native modules, NOT Lua (A5 below). | plan A5 rule, measured |
| D5 | Stored format: pi message model and JSONL session v3; local timestamps without offset (E9). | plan, round 2 item 1 |
| D6 | Permissions: pi default, never ask; C:Claude's gate is a built-in extension, off by default. | plan |
| D7 | Providers: Anthropic, OpenAI-compatible (phase B), then Google, Azure, Bedrock (SigV4 and API keys); subscription logins last, gated on probe O1 (still open). | plan, round 2 |
| D8 | B5 stays: C:Claude's `http.c` head and `stream.c` decoder move onto the shared backend, gated by goldens. | owner, round 2 item 3 |
| D9 | `llm/` lives in THIS repo (two repos cannot share a top-level dir the plan puts in vtcon). B5 therefore needs vtcon's Makefile to compile `$(AMIGA_PI_DIR)/llm/*.c`. That touches vtcon and is not done in phase A; it is flagged for the owner. | this task, open |

## Layers and headers

```
UI            amiga-pi/ui_*.c, tui reuse (claude/tui.h, input.c, keys.c)     modes.h
agent         amiga-pi/agent.h (pi_prompt), ext.h, session3.h
LLM           llm/llm.h (messages), llm/event.h, llm/backend.h, llm/request.c, llm/be_*.c
HTTP          claude/http.h   (VTCON_DIR)
SSE / JSON    claude/sse.h, claude/json.h   (VTCON_DIR)
TLS           claude/tls.h    (tls_amissl.c | tls_none.c)
transport     claude/net.h    (net_amiga.c bsdsocket | net_posix.c host | test stubs)
```

Rule, enforced by `tools/check_layers.py` (A4, `make test`): UI -> agent -> LLM -> HTTP -> TLS -> bsdsocket, one direction only.
- `llm/` includes no `ui.h`, `tui.h`, `sys.h`; `llm/be_*.c` also no AmigaDOS headers.
- `amiga-pi/` UI files include no `net.h`/`tls.h`.
- `claude/http.c` (vtcon) contains no provider name. It does today (5 lines, Anthropic head and `x-api-key` redaction, `http.c:112-142`); the check reports it as PENDING(B5) and fails under `--strict` once B5 should have landed.

## Data flow of one turn

prompt -> context build (system sections + project instructions + active branch) -> backend body (`llm_backend.body`) -> HTTP request (head from the backend, key never on plain http) -> response bytes -> `sse_feed` -> backend `feed` -> neutral events (`start`, `text|thinking|toolcall` start/delta/end by contentIndex, `done|error`) -> accumulator (`llm/accum.c`) -> assistant message entry appended to the session leaf -> tool calls run (sequentially) -> toolResult entries -> next round until stopReason != toolUse. Failures travel inside the event stream (`error{aborted|error, partial}`), never as a return path the caller special-cases (pi-ai behaviour, R2.1).

## Memory model

Single Amiga heap, no paging. The session INDEX (entry id, parentId, type, file offset, token estimate) is in RAM for the whole file; only the ACTIVE BRANCH's messages are loaded as JSON text (R3.2). Peak heap per session was never measured in C:Claude ([unconfirmed], R1.2): first measurement is a phase C item. Stack cookie 32768 as C:Claude (`main_amiga.c:53`); JSON parsing is non-recursive (depth 64).

## Execution model

Single-threaded. pi may run the tool calls of one message in parallel; amiga-pi runs them one after the other in message order (one task, one `sys->run`). Ctrl+C aborts the current request or tool (stopReason `aborted`, partial content kept).

## LAN rule

`http://` is accepted only if the address `cl_net` actually connected to is loopback (127/8), private (10/8, 172.16/12, 192.168/16) or link-local (169.254/16); the name is not trusted. On `http://` no credential header is written; a configured key is dropped with one notice per provider per run ("key for <provider> not sent: plain http"). pi's `apiKey: "ollama"` examples therefore keep working. `https://` is unrestricted.

## Directories

- Agent dir `ENVARC:amiga-pi/` (pi: `~/.pi/agent/`): `settings.json`, `models.json`, `auth.json`, `keybindings.json`, `sessions/--<cwd slug>--/<timestamp>_<id>.jsonl`, `skills/`, `prompts/`, `themes/`, `extensions/`, `SYSTEM.md`, `APPEND_SYSTEM.md`, `AGENTS.md`.
- Project dir `<root>/.pi/` (pi's name kept so a project works with pi on a Mac): same resource folders, trust-gated (phase G).
- Temp: `T:amiga-pi-<id>.txt` for truncated tool output.

## Shared with C:Claude / owned by each

| Shared (compiled in place from VTCON_DIR, not copied) | C:Claude owns | amiga-pi owns |
|---|---|---|
| `net.h`, `net_amiga.c`, `net_posix.c`, `tls.h`, `tls_amissl.c`, `tls_none.c`, `http.c`, `sse.c`, `json.c`, `util.c`, `sys.h`/`sys_*.c`, `tools.c` (read/write/edit/bash/grep/find implementations), `policy.c`, `hooks.c`, `tui.c`/`input.c`/`keys.c` | `repl.c`, `conv.c` (Anthropic message store), `session.c` (Claude Code format), `print.c` (Agent SDK shapes), `commands.c`, `slash.c`, server tools, remote mode | `llm/` (neutral model, events, backends), `amiga-pi/` (agent loop, session3, extensions, modes, tool name/schema mapping `tools_pi.c`) |

Until B5, C:Claude keeps its own Anthropic decoder; after B5 both programs consume `llm/`. Build of both programs and the golden gate belong to B (not phase A).

## Tools

pi names: `read`, `write`, `edit`, `grep`, `find`, `ls`, and `bash`. Per D3 `bash` runs `vsh -c` when installed, else the AmigaDOS Shell, and the system prompt states which one is active (a model told "bash" writes Unix commands; vsh makes that true, the fallback does not). Session roles and RPC names keep pi's spelling (`bashExecution`, `bash`, `abort_bash`, `user_bash`). `edit` is pi's multi-edit (all oldText matched against the original, unique, non-overlapping, all or nothing). Truncation 2000 lines / 50 KB, full output to `T:`.

## Extensions (decision D4, probe A5)

Plan rule: Lua 5.4 is chosen only if all four criteria hold.

Measured on the Mac (2026-10-07), Lua 5.4.9 vendored in `vendor/lua-5.4/`, vbcc via vtcon's cfg, `-cpu=68020 -O2 -warnings-as-errors`, `LUA_USE_C89` + `LUA_32BITS`, probe built by `tools/probe/build_lua_probe.sh`:

| # | Criterion | Result |
|---|---|---|
| 1 | builds with `-warnings-as-errors`, changes only in `luaconf.h`, at most 5 `-dontwarn` numbers | FAILS. Compiles cleanly (0 warning lines, 0 failures) only with 10 numbers: 65, 79, 153, 163, 166, 167, 170, 172, 306, 307 (163 is vbcc's own `stdio.h` pragma, 306/307 are struct padding/alignment, 153 is Lua's `((void)0)` macros). Un-suppressed, 120/129/170 also fire (120 cannot be suppressed at all: `error 160`); those I removed by luaconf-only edits (empty `LUA_API`/`LUAI_FUNC`, `l_sprintf`, `luai_numidiv`, `luai_numpow`). Even excluding 163, 9 numbers remain. |
| 2 | Amiga output equals host output byte for byte | NOT RUN (rig half). Host output recorded in `tools/probe/luaprobe.expected`; the host build uses the same `LUA_32BITS`. |
| 3 | probe adds at most 300 KB to a hello world | holds: luaprobe 187,572 bytes, hello 4,892 bytes, delta 182,680 bytes (libs open: base, coroutine, table, string, math, utf8; io, os, package, debug not linked; link needs `-lmieee`, i.e. mathieeedoubtrans.library at run time). |
| 4 | fib(24) plus 100,000-iteration loop within 10 s on a 68020 without FPU | NOT RUN (rig half). |

**Decision: native modules.** Criterion 1 is definitively missed (needs all four); criteria 2 and 4 cannot change that. Extensions are LoadSeg'ed hunk files built with vbcc against `amiga-pi/ext_api.h`, exporting one function table; hook commands (`claude/hooks.c`) come first (phase L). Consequence: no scripting for users without a C toolchain; pi's TypeScript extensions are not portable in any case. The owner can overrule on the 10-versus-5 count (the plan's 5 was a choice, not a measurement); then only 2 and 4 remain to run. `vendor/lua-5.4/` stays as the evidence and for a later reversal.

## What is not in phase A

No product code exists yet: no `llm/`, `amiga-pi/` sources, no program builds. Phase A delivers this document, COMPATIBILITY.md, the addendum, the layering check, the Lua probe and the ledger (`progress.md`).
