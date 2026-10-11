# amiga-pi architecture (milestone 0)

Plan: `~/Code/vtcon/thoughts/shared/plans/2026-10-07-amiga-pi.md` (phase A). Research: `~/Code/vtcon/thoughts/shared/research/2026-10-07_pi-like-amiga-agent.md` (R-numbers below). pi source facts: `thoughts/shared/research/2026-10-07_pi-sources-addendum.md` (pi commit f10993bc7f28145df1375f3ff39c7f5c4cfc05f0). Nothing here ran on an Amiga.

## Decisions recorded (owner, 2026-10-07)

| Id | Decision | Source |
|---|---|---|
| D1 | amiga-pi lives in its own LOCAL repo `~/Code/amiga-pi` (no remote, no push). | owner, this task |
| D2 | It shares C:Claude's core and `llm/` by REFERENCE: sources are compiled in place from `$(VTCON_DIR)` (default `$(UPTERM_ROOT)/vtcon`, `UPTERM_ROOT` default `~/Code`), never copied (plan B5 reuse rule). | owner |
| D3 | The shell tool is named `bash` and runs `vsh -c` (the kit's bash-level shell); AmigaDOS Shell only as fallback when vsh is absent, and the model is told which. | owner, round 2 item 5 |
| D4 | Extension language: Lua 5.4. The owner chose Lua on 2026-10-07 evening ("Lua now"), overriding the native-modules call from A5's criterion 1 (the 5-warning limit was a guess). The Amiga run of the probe is a check, not a gate (Extensions below). | owner 2026-10-07 evening, again 2026-10-10 |
| D5 | Stored format: pi message model and JSONL session v3; local timestamps without offset (E9). | plan, round 2 item 1 |
| D6 | Permissions: pi default, never ask; C:Claude's gate is a built-in extension, off by default. | plan |
| D7 | Providers: Anthropic, OpenAI-compatible (phase B), then Google, Azure, Bedrock (SigV4 and API keys); subscription logins last, gated on probe O1 (still open). | plan, round 2 |
| D8 | B5 stays: C:Claude's `http.c` head and `stream.c` decoder move onto the shared backend, gated by goldens. | owner, round 2 item 3 |
| D9 | `llm/` lives in vtcon, next to `claude/` (`~/Code/vtcon/llm/`). C:Claude links it from its own tree, so its build stays inside one repo and B5 is a single-repo change gated by vtcon's request goldens; amiga-pi compiles it in place from `$(VTCON_DIR)` like the rest of the shared core. | owner 2026-10-10 (option a) |
| D10 | Growing and bulky data (sessions, skills, prompts, themes, extensions) in `UP-Term:var/amiga-pi/`, small config in `ENVARC:amiga-pi/` (Directories below). | coordinator review 2026-10-10 |

## Layers and headers

```
UI            amiga-pi/ui_*.c, tui reuse (claude/tui.h, input.c, keys.c)     modes.h
agent         amiga-pi/agent.h (pi_prompt), ext.h, session3.h
LLM           llm/llm.h (messages), llm/event.h, llm/backend.h, llm/request.c, llm/be_*.c   (VTCON_DIR)
HTTP          claude/http.h   (VTCON_DIR)
SSE / JSON    claude/sse.h, claude/json.h   (VTCON_DIR)
TLS           claude/tls.h    (tls_amissl.c | tls_none.c)
transport     claude/net.h    (net_amiga.c bsdsocket | net_posix.c host | test stubs)
```

Rule, enforced by vtcon's `tools/check_layers.py` (A4; in vtcon's `make test` and this repo's `make test`): UI -> agent -> LLM -> HTTP -> TLS -> bsdsocket, one direction only.
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

Owner rule (2026-10-07): `http://` is accepted only for a LAN host: an address in 127/8, 10/8, 172.16/12 or 192.168/16, or a name ending in `.local`; anything else is refused before connecting, with a message that says to use https. The address checked is the one the name resolved to (`cl_net` connects to it), so a public name resolving to a private address counts as LAN. On `http://` no credential header is written; a configured key is dropped with one notice per provider per run ("key for <provider> not sent: plain http"). pi's `apiKey: "ollama"` examples therefore keep working. `https://` is unrestricted. Link-local 169.254/16 is not in the owner's list and is refused.

## Directories

- Config dir `ENVARC:amiga-pi/` (pi: `~/.pi/agent/`), small files only: `settings.json`, `models.json`, `auth.json`, `keybindings.json`, `SYSTEM.md`, `APPEND_SYSTEM.md`, `AGENTS.md` (as C:Claude's `ENVARC:Claude/`: key, remote, settings.json, CLAUDE.md).
- Data dir `UP-Term:var/amiga-pi/` (decision D10): `sessions/--<cwd slug>--/<YYYYMMDDTHHMMSS>_<8 hex>.jsonl` (the slug is pi's cut to 30 characters by C:Claude's rule; the file name is 30 characters; see COMPATIBILITY.md, sessions), `skills/`, `prompts/`, `themes/`, `extensions/`. The `UP-Term:` assign is probed without a requester (`cl_sys.quiet_kind`); without it (no kit, or before the assign exists), or when `UP-Term:var/amiga-pi` cannot be made, the data dir is `ENVARC:amiga-pi/` and nothing moves. `PI_CODING_AGENT_DIR` keeps everything in that directory. The rule and the path live in one place, vtcon `claude/datadir.c` (`dd_dir`), shared with C:Claude, whose growing files (`projects/`, `agent-memory/`, `skills/`, `session.json`, `history`) moved the same way to `UP-Term:var/Claude/` with a move-on-first-run migration.
- Why (checked 2026-10-10): Workbench 3.1's `S:Startup-Sequence` (rig 2, lines 11-17) runs `Copy ENVARC: RAM:ENV ALL` at every boot and assigns `ENV:` there, so every session file under `ENVARC:` would be loaded into RAM on each boot. 3.2's (`S/Startup-sequence` line 21 of the rig's 3.2 tree) links `RAM:ENV` to `ENVARC:` instead (`MakeLink ... Force`), which costs no RAM but makes `ENV:` writes persistent. `UP-Term:` is the kit's own drawer (the Installer's `Assign UP-Term:` in `S:User-Startup`) and already holds `bin/`, `share/`, `etc/`; `var/` follows that Unix layout, so no new assign. Known cost: the kit's Uninstall deletes `UP-Term:` with everything in it, sessions included.
- Project dir `<root>/.pi/` (pi's name kept so a project works with pi on a Mac): same resource folders, trust-gated (phase G).
- Temp: `T:amiga-pi-<id>.txt` for truncated tool output.

## Shared with C:Claude / owned by each

| Shared (in vtcon, compiled in place from VTCON_DIR, not copied) | C:Claude owns | amiga-pi owns |
|---|---|---|
| `llm/` (neutral model, events, accumulator, backends, request, pool, LAN rule), `net.h`, `net_amiga.c`, `net_posix.c`, `tls.h`, `tls_amissl.c`, `tls_none.c`, `http.c`, `sse.c`, `json.c`, `util.c`, `sys.h`/`sys_*.c`, `tools.c` (read/write/edit/bash/grep/find implementations), `policy.c`, `hooks.c`, `tui.c`/`input.c`/`keys.c` | `repl.c`, `conv.c` (Anthropic message store), `session.c` (Claude Code format), `print.c` (Agent SDK shapes), `commands.c`, `slash.c`, server tools, remote mode | `amiga-pi/` (agent loop, session3, extensions, modes, tool name/schema mapping `tools_pi.c`) |

Until B5, C:Claude keeps its own Anthropic decoder; after B5 both programs consume `llm/`. Build of both programs and the golden gate belong to B (not phase A).

## Tools

pi names: `read`, `write`, `edit`, `grep`, `find`, `ls`, and `bash`. Per D3 `bash` runs `vsh -c` when installed, else the AmigaDOS Shell, and the system prompt states which one is active (a model told "bash" writes Unix commands; vsh makes that true, the fallback does not). Session roles and RPC names keep pi's spelling (`bashExecution`, `bash`, `abort_bash`, `user_bash`). `edit` is pi's multi-edit (all oldText matched against the original, unique, non-overlapping, all or nothing). Truncation 2000 lines / 50 KB, full output to `T:`.

As built (phase D, 2026-10-11): `amiga-pi/tools_pi.c` holds pi's names, descriptions and schemas and maps each call onto C:Claude's tool (`tools_run` on one `cl_tools` with `pi_rules` set: no Read before Write, missing directories made, a command failed when its code is not 0, pi's status words after its output, the output kept whole up to 512 KB for the truncation). `edit` is `claude/edit_orig.c` (`tools_edit_orig`, on Edit's loading and write; linked by amiga-pi only, `CLAUDE_PI` in vtcon's sources.mk). `ls` is the one tool with its own code (a lister over `sys->list`; C:Claude has none). The bash tool runs through `sys->run` as C:Claude's Bash does: sys_amiga writes the command to a script and runs `vsh <script>` (UP-Term:bin/vsh or C:vsh), else `Execute` in the AmigaShell; every command starts with a `cd` to the working directory. Truncation is `amiga-pi/truncate.c` (`pi_cut`), one function for every tool. The permission gate is `amiga-pi/ext_gate.c` (C:Claude's rules and question, off by default).

As built (phase E, 2026-10-11): the session is `amiga-pi/session3.c` (pi's JSONL v3: writer, streaming reader with an index and only the active branch in RAM, migration, branched sessions, file discovery), `context.c` (the context of a branch: compaction, context edits, convertToLlm, the replayed system message), `compact.c` (token estimates, cut points, prepareCompaction, compact, branch summaries, with pi's prompts generated into `cp_prompts.c`), `store.c` (the glue to the agent: persistence on message_end, the system prompt in the transcript, the automatic compaction hook `pi_session.compact`, called at pi's points `PI_CP_*`, opening and restoring) and `cmds.c` (`/tree /fork /clone /label /name /session`; `/compact` is in store.c). The agent keeps the model-visible messages in `pi_session.msg`; the tree is the store's. Everything is portable C89; the oracle (`tools/pi_oracle/`) regenerates the expected outputs from pi's own code.

## Extensions (decision D4, probe A5)

**Decision (owner, 2026-10-07 evening, confirmed 2026-10-10): Lua 5.4.** The plan's four criteria were written as a gate; the owner made them a check ("Lua now"; criterion 1's 5-warning limit was a guess). The numbers below are kept as information.

Measured on the Mac (2026-10-07), Lua 5.4.9 vendored in `vendor/lua-5.4/`, vbcc via vtcon's cfg, `-cpu=68020 -O2 -warnings-as-errors`, `LUA_USE_C89` + `LUA_32BITS`, probe built by `tools/probe/build_lua_probe.sh`:

| # | Criterion | Result |
|---|---|---|
| 1 | builds with `-warnings-as-errors`, changes only in `luaconf.h`, at most 5 `-dontwarn` numbers | FAILS. Compiles cleanly (0 warning lines, 0 failures) only with 10 numbers: 65, 79, 153, 163, 166, 167, 170, 172, 306, 307 (163 is vbcc's own `stdio.h` pragma, 306/307 are struct padding/alignment, 153 is Lua's `((void)0)` macros). Un-suppressed, 120/129/170 also fire (120 cannot be suppressed at all: `error 160`); those I removed by luaconf-only edits (empty `LUA_API`/`LUAI_FUNC`, `l_sprintf`, `luai_numidiv`, `luai_numpow`). Even excluding 163, 9 numbers remain. |
| 2 | Amiga output equals host output byte for byte | holds (rig 2, 2026-10-10): `RAM:luaprobe/out.txt` equals `tools/probe/luaprobe.expected` (cmp). First run failed (`malformed number near '3.14159'`, `%.2f` printed literally) because the probe linked `-lvc` before `-lmieee`, so vc.lib's integer-only printf/strtod won; fixed in `build_lua_probe.sh` (math library first). |
| 3 | probe adds at most 300 KB to a hello world | holds: luaprobe 193,160 bytes (after the link-order fix; 187,572 before), hello 4,892 bytes, delta 188,268 bytes (libs open: base, coroutine, table, string, math, utf8; io, os, package, debug not linked; link needs `-lmieee`, i.e. mathieeedoubtrans.library at run time). |
| 4 | fib(24) plus 100,000-iteration loop within 10 s on a 68020 without FPU | FAILS (rig 2, 2026-10-10, AmigaDOS `Date` around each run, 1 s resolution): loop 65 s, fib(24) 27 s, whole probe 91 s -- 9 times the limit. Rig 2 is FS-UAE A1200, 68020 at `uae_cpu_speed = real`, not cycle-exact, and has a 68882 configured (the plan assumed no FPU; both parts are integer work). |

What the numbers mean for the design: the speed was measured on a plain 68020 at real A1200 speed (rig 2's configuration then), the slowest machine the kit supports; a 68060 is several times faster. Extensions should therefore do event handling and small transforms in Lua and leave heavy work (search, file walking, JSON of large payloads) to the C tools they call. The link needs `-lmieee` BEFORE `-lvc`. Next Lua step (owner, 2026-10-07): evaluate AmigaDaLua (https://github.com/alpine9000/AmigaDaLua) against the vendored stock 5.4.9 and reuse it if it builds with vbcc and its licence and additions fit (phase L). Hook commands (`claude/hooks.c`) still come first (phase L).

## What is not in phase A

No product code existed at the end of phase A: no `llm/` (now in vtcon, phase B), no `amiga-pi/` sources, no program builds. Phase A delivers this document, COMPATIBILITY.md, the addendum, the layering check, the Lua probe and the ledger (`progress.md`).
