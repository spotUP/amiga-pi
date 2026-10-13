# amiga-pi architecture (milestone 0)

Plan: `~/Code/vtcon/thoughts/shared/plans/2026-10-07-amiga-pi.md` (phase A). Research: `~/Code/vtcon/thoughts/shared/research/2026-10-07_pi-like-amiga-agent.md` (R-numbers below). pi source facts: `thoughts/shared/research/2026-10-07_pi-sources-addendum.md` (research addendum read pi commit f10993bc, a main snapshot between 1.0.4 and 1.1.0; since 2026-10-12 the reference is **pi 1.1.0 = tag `v1.1.0`, commit abe508e1b89912adde45528136c3221eb69acdd7**, github.com/earendil-works/pi, cloned in `~/Code/amiga-pi/vendor/pi-src` (gitignored): `git clone --branch v1.1.0 https://github.com/earendil-works/pi vendor/pi-src`; read pi code there, not on the web). Nothing here ran on an Amiga.

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

## MCP stdio pipes (K1, measured on rig 3, 2026-10-11)

Probe: vtcon `tests/amiga/spawnprobe.c` (`make build/amiga/spawnprobe`, run as `VTC:spawnprobe` on the rig; FS-UAE A1200 --max, Workbench 3.1, PTY: from vtcon's pty-handler). A native (vbcc, no ixemul) parent starts `spawnprobe child` with `SystemTags(SYS_Input, SYS_Output, SYS_Asynch)` on two pipes, writes `ping\n`, sends one 4096-byte ACTION_READ on the child's output, then closes its write end. The child reads 4096 at a time and answers each read with one line.

| handler | child's stdin gets the line | parent gets the child's line | EOF after the parent's close | verdict |
|---|---|---|---|---|
| `PIPE:` (one name, both ends) | no: the child's 4096-byte Read waits until the writer closes (its log is empty after 3 s) | no answer in 3 s | yes (the held bytes, then 0) | unusable: a request/answer protocol deadlocks |
| `PTY:<id>/w` + `PTY:<id>/r` (pty-handler pipe pair) | yes | yes, `echo 5\n` at once | yes (`eof\n`, then 0) | **chosen** |
| `IXPIPE:` | - | - | - | not usable from a native program: an ordinary Open fails by design (ixpipe-handler.c only answers ACTION_IXEMUL_MAGIC, which clones a descriptor of an ixemul process, F_INTERNALIZE); amiga-pi links no ixemul |

So `spawn2` on the Amiga (K5) makes two PTY: pipe pairs, `PTY:mcpi<n>/w|r` (to the child) and `PTY:mcpo<n>/w|r` (from it). PTY: is the kit's (the Installer mounts PTY:, XCON:, IXPIPE:); without it a stdio server is refused with a message naming PTY:, and HTTP servers still work. What the server's own C library buffers is the server's business: a server must flush each answer line, as on any OS.

## Shared with C:Claude / owned by each

| Shared (in vtcon, compiled in place from VTCON_DIR, not copied) | C:Claude owns | amiga-pi owns |
|---|---|---|
| `llm/` (neutral model, events, accumulator, backends, request, pool, LAN rule), `net.h`, `net_amiga.c`, `net_posix.c`, `tls.h`, `tls_amissl.c`, `tls_none.c`, `http.c`, `sse.c`, `json.c`, `util.c`, `sys.h`/`sys_*.c`, `tools.c` (read/write/edit/bash/grep/find implementations), `policy.c`, `hooks.c`, `tui.c`/`input.c`/`keys.c` | `repl.c`, `conv.c` (Anthropic message store), `session.c` (Claude Code format), `print.c` (Agent SDK shapes), `commands.c`, `slash.c`, server tools, remote mode | `amiga-pi/` (agent loop, session3, extensions, modes, tool name/schema mapping `tools_pi.c`) |

Until B5, C:Claude keeps its own Anthropic decoder; after B5 both programs consume `llm/`. Build of both programs and the golden gate belong to B (not phase A).

## Tools

pi names: `read`, `write`, `edit`, `grep`, `find`, `ls`, and `bash`. Per D3 `bash` runs `vsh -c` when installed, else the AmigaDOS Shell, and the system prompt states which one is active (a model told "bash" writes Unix commands; vsh makes that true, the fallback does not). Session roles and RPC names keep pi's spelling (`bashExecution`, `bash`, `abort_bash`, `user_bash`). `edit` is pi's multi-edit (all oldText matched against the original, unique, non-overlapping, all or nothing). Truncation 2000 lines / 50 KB, full output to `T:`.

As built (phase D, 2026-10-11): `amiga-pi/tools_pi.c` holds pi's names, descriptions and schemas and maps each call onto C:Claude's tool (`tools_run` on one `cl_tools` with `pi_rules` set: no Read before Write, missing directories made, a command failed when its code is not 0, pi's status words after its output, the output kept whole up to 512 KB for the truncation). `edit` is `claude/edit_orig.c` (`tools_edit_orig`, on Edit's loading and write; linked by amiga-pi only, `CLAUDE_PI` in vtcon's sources.mk). `ls` is the one tool with its own code (a lister over `sys->list`; C:Claude has none). The bash tool runs through `sys->run` as C:Claude's Bash does: sys_amiga writes the command to a script and runs `vsh <script>` (UP-Term:bin/vsh or C:vsh), else `Execute` in the AmigaShell; every command starts with a `cd` to the working directory. Truncation is `amiga-pi/truncate.c` (`pi_cut`), one function for every tool. The permission gate is `amiga-pi/ext_gate.c` (C:Claude's rules and question, off by default).

As built (phase E, 2026-10-11): the session is `amiga-pi/session3.c` (pi's JSONL v3: writer, streaming reader with an index and only the active branch in RAM, migration, branched sessions, file discovery), `context.c` (the context of a branch: compaction, context edits, convertToLlm, the replayed system message), `compact.c` (token estimates, cut points, prepareCompaction, compact, branch summaries, with pi's prompts generated into `cp_prompts.c`), `store.c` (the glue to the agent: persistence on message_end, the system prompt in the transcript, the automatic compaction hook `pi_session.compact`, called at pi's points `PI_CP_*`, opening and restoring) and `cmds.c` (`/tree /fork /clone /label /name /session`; `/compact` is in store.c). The agent keeps the model-visible messages in `pi_session.msg`; the tree is the store's. Everything is portable C89; the oracle (`tools/pi_oracle/`) regenerates the expected outputs from pi's own code.

As built (phase K, 2026-10-11): MCP is `amiga-pi/mcp.c` (pi's mcp.json shape and validation, the JSON-RPC client, tool names, results as model content with the 20 KB middle cut kept in `T:`, the exposures, `tool_search` and the `mcp_servers` section) over two transports: `mcp_http.c` on vtcon's `llm_post` (its `header` and `body` hooks carry Mcp-Session-Id and application/json answers; event streams through `sse.c`; the LAN rule) and `mcp_stdio.c` on `cl_sys` spawn2 (vtcon `claude/sys.h`; `sys_posix.c` fork/exec on two pipes, `sys_amiga.c` two PTY: pipe pairs with pending ACTION_READ packets, see "MCP stdio pipes"). `pi_app` reads `<agent dir>/mcp.json` and connects every enabled server before the first prompt (`--no-mcp` skips it); direct tools join the session's tools, deferred ones wait for `tool_search`. No floating point: BM25 runs in fixed point (`ln` by its atanh series, a 64-bit `mul_div`), so amiga-pi still needs no math library. Host tests: `tests/test_pi_mcp.c` against `tools/fake_mcp.py` (stdio and HTTP) and pi's own outputs (`tools/pi_oracle/mcp_oracle.sh`); on the Amiga: vtcon `tools/rig/amigapi_rig.py --step 10` (a server on the Mac over LAN http and `build/amiga/mcp-echo` over PTY:).

As built (phase F, 2026-10-12): the model registry lives in vtcon's `llm/`: `registry.c` (arena-owned `llm_model` list, the builtin catalog generated from `models_builtin.json` into `models_builtin.h` by `tools/gen_models_builtin.py` (`--from-pi` from pi 1.1.0, abe508e1, through `tools/dump_pi_models.mjs` against `vendor/pi-1.1.0`; classifier models are not in it, pi does not list them either), pi's model resolution and thinking-level support/clamp), `models_json.c` (models.json in pi's shape as an overlay on the registry), `creds.c` (config values `$NAME`, `${NAME}`, literal, `!command` run once per process through `cl_sys->run`; auth.json; credential order --api-key, auth.json, models.json, environment) and `handoff.c` (pi's transformMessages first pass, then `llm_transform`; one entry `llm_prepare`, the backends do no handoff of their own). Here `amiga-pi/models.c` binds the registry to the platform (files through `cl_sys`, the command runner), `cmds.c` holds `/model` (numbered list, `/model <number|provider/pattern>`) and `/thinking`, `agent.c` `pi_req_auth` resolves key and headers per request, and `app.c` writes `model_change` and `thinking_level_change` entries and restores the thinking level on resume. Thinking reaches the wire in `be_anthropic_pi.c` (budget, adaptive plus effort, disabled) and `be_openai_chat.c` (`reasoning_effort`). `be_openai_responses.c` (openai-responses, FA4; the api of pi's openai models gpt-5.5 and gpt-4.1) is pi-ai's provider ported: the body (instructions role developer or system, input items with `fc_` ids and foreign thinking handed off as text, tools with `strict:false`, `reasoning` effort from the map plus `summary:auto` and `include`, `store:false`, `prompt_cache_key` and the session headers from `pi_session.session_id`), the SSE decoder (output items to blocks, `error`/`failed`/`incomplete` as pi's messages) and the text signature `{v:1,id,phase}`; its goldens (tests/pi/responses in vtcon) are made by pi-ai's own `api/openai-responses.js` (`tools/gen_pi_responses.mjs`), the switch goldens by `tools/pi_oracle/gen_switch.mjs`. Reachability: `tests/test_pi_model_reach.c` drives `pi_app`, `/model` and a second prompt (goldens for both bodies, sentinels in both backends, the key command run once); on the rig `tools/rig/amigapi_rig.py --step 6`.

As built (phase M, batch 1, 2026-10-13): two more backends in vtcon's `llm/`, registered in `llm_backend_of`. `be_google.c` (google-generative-ai; pi's `api/google-generative-ai.ts` and `google-shared.ts`, the request as the @google/genai SDK writes it): the path depends on the model (new `llm_backend.route`), the stream has no terminal event (new `llm_backend.fin` closes it when the body ends), a block's signature is replaced, not appended (`llm_event.replace`), a tool call keeps its thoughtSignature, generated call ids read the host's clock (`llm_set_wall`, set by `pi_req_auth`). `be_azure.c` (azure-openai-responses) is the Responses backend with the deployment as model, an `api-key` header and `?api-version=`; `llm_cfg_endpoint` (creds.c) resolves AZURE_OPENAI_* onto the model before each request, also in `pi_req_auth`. Goldens made by pi 1.1.0 itself: `tests/pi/google` (`tools/gen_pi_google.mjs`), `tests/pi/azure` (`tools/gen_pi_responses.mjs azure`), `tests/pi/handoff` (`tools/gen_pi_handoff.mjs`: every ordered pair of six models). Reachability: `tests/test_pi_model_reach.c` test_google_reach / test_azure_reach through `pi_app`, sentinels `llm_feeds_google` and `llm_feeds_azure`.

As built (phase M, batch 2, 2026-10-13): Amazon Bedrock. `llm/evstream.c` decodes the AWS event-stream framing (prelude and message CRC-32, pieces of any size; `request.c` hands each message to the backend like an SSE event, an exception as `!<type>`), `llm/sigv4.c` signs with Signature Version 4 on `llm/sha256.c` (moved from amiga-pi: HMAC-SHA256 without a crypto library), `llm/be_bedrock.c` is the converse-stream backend (pi's `api/bedrock-converse-stream.ts`, the request as the SDK's restJson1 serializer writes it). New backend hooks: `sign` (headers that need the finished request, called by `llm_request` after the body is built; `bearer` tells it a bearer token is configured), `http_err` (the SDK's wording of a failed answer, with the `x-amzn-errortype` header); `llm_cfg_endpoint` resolves region, endpoint and the AWS_* credentials onto the model (`llm_aws`), `llm_cfg_authed` is pi's checkAuth for them. Goldens made by pi 1.1.0 and its SDK itself: `tests/pi/bedrock` (`tools/gen_pi_bedrock.mjs`, node:https redirected to a local server by `tools/pi_bedrock_oracle.mjs`, so the SDK runs its real endpoint and signing code), `tests/pi/evstream` (`tools/gen_pi_evstream.mjs`, the SDK's codec), `tests/pi/sigv4` (AWS's published suite), `tests/pi/handoff` (now eight models, all 30 ordered api pairs). Reachability: `tests/test_pi_model_reach.c` test_bedrock_reach through `pi_app`, sentinel `llm_feeds_bedrock`.

## Extensions (decision D4, probe A5)

**Decision (owner, 2026-10-07 evening, confirmed 2026-10-10): Lua 5.4.** The plan's four criteria were written as a gate; the owner made them a check ("Lua now"; criterion 1's 5-warning limit was a guess). The numbers below are kept as information.

Measured on the Mac (2026-10-07), Lua 5.4.9 vendored in `vendor/lua-5.4/`, vbcc via vtcon's cfg, `-cpu=68020 -O2 -warnings-as-errors`, `LUA_USE_C89` + `LUA_32BITS` (the A5 build; L13 later went to 64-bit numbers, below), probe built by `tools/probe/build_lua_probe.sh`:

| # | Criterion | Result |
|---|---|---|
| 1 | builds with `-warnings-as-errors`, changes only in `luaconf.h`, at most 5 `-dontwarn` numbers | FAILS. Compiles cleanly (0 warning lines, 0 failures) only with 10 numbers: 65, 79, 153, 163, 166, 167, 170, 172, 306, 307 (163 is vbcc's own `stdio.h` pragma, 306/307 are struct padding/alignment, 153 is Lua's `((void)0)` macros). Un-suppressed, 120/129/170 also fire (120 cannot be suppressed at all: `error 160`); those I removed by luaconf-only edits (empty `LUA_API`/`LUAI_FUNC`, `l_sprintf`, `luai_numidiv`, `luai_numpow`). Even excluding 163, 9 numbers remain. |
| 2 | Amiga output equals host output byte for byte | holds (rig 2, 2026-10-10): `RAM:luaprobe/out.txt` equals `tools/probe/luaprobe.expected` (cmp). First run failed (`malformed number near '3.14159'`, `%.2f` printed literally) because the probe linked `-lvc` before `-lmieee`, so vc.lib's integer-only printf/strtod won; fixed in `build_lua_probe.sh` (math library first). |
| 3 | probe adds at most 300 KB to a hello world | holds: luaprobe 193,160 bytes (after the link-order fix; 187,572 before), hello 4,892 bytes, delta 188,268 bytes (libs open: base, coroutine, table, string, math, utf8; io, os, package, debug not linked; link needs `-lmieee`, i.e. mathieeedoubtrans.library at run time). |
| 4 | fib(24) plus 100,000-iteration loop within 10 s on a 68020 without FPU | FAILS (rig 2, 2026-10-10, AmigaDOS `Date` around each run, 1 s resolution): loop 65 s, fib(24) 27 s, whole probe 91 s -- 9 times the limit. Rig 2 is FS-UAE A1200, 68020 at `uae_cpu_speed = real`, not cycle-exact, and has a 68882 configured (the plan assumed no FPU; both parts are integer work). |

L13 (owner 2026-10-13, 64-bit numbers): `LUA_32BITS` is off, so Lua has `long long` integers and `double` floats as pi's JS numbers need (integers to 2^53 exact; JSON numbers round-trip through a handler, `json.encode` writes the double as JSON.stringify does, `pi_js_double` in mcp.c). The probe still builds clean with the same `-dontwarn` list (0 warning lines); luaprobe grew 193,160 to 202,924 bytes, the Amiga binary 1,559,668 to 1,568,732 (+9,064); `luaprobe.expected`'s `loop` line is now 5000050000 (the 32-bit sum wrapped to 705082704). Rig 2 re-measure (2026-10-13, the same stock A1200 configuration, 1 s `Date` resolution, output byte-equal to `luaprobe.expected`): loop 72 s (was 65), fib(24) 30 s (27), whole probe 102 s (91): about 10 % slower.

What the numbers mean for the design: the speed was measured on a plain 68020 at real A1200 speed (rig 2's configuration then), the slowest machine the kit supports; a 68060 is several times faster. Extensions should therefore do event handling and small transforms in Lua and leave heavy work (search, file walking, JSON of large payloads) to the C tools they call. The link needs `-lmieee` BEFORE `-lvc`. Phase L3 built it in (next section). Next Lua step (owner, 2026-10-07): evaluate AmigaDaLua (https://github.com/alpine9000/AmigaDaLua) against the vendored stock 5.4.9 and reuse it if it builds with vbcc and its licence and additions fit (phase L). Hook commands (`claude/hooks.c`) still come first (phase L).

## Lua extensions (Phase L3)

`amiga-pi/ext_lua.c` (portable C89, host-tested in `tests/test_pi_lua.c`) runs pi's extension API in Lua 5.4 (`vendor/lua-5.4`,
stock 5.4.9; the changes are in `luaconf.h` only: A5's `LUA_USE_C89`, L13's 64-bit numbers (`LUA_32BITS 0`, `LUA_C89_NUMBERS 0`: `long long` and `double`) and, new, `LUAI_MAXCCALLS 60` because the
C stack is 32 KB). `pi_rt_open` loads, after the trust decision, `<cwd>/.pi/extensions` (trusted projects only), then
`<agent dir>/extensions` and the data dir's (`*.lua`, `<dir>/index.lua`; name order; pi: loader.ts discoverAndLoadExtensions);
`--no-extensions` skips it. Each file is a Lua state of its own with a memory cap and an instruction and clock limit
(`PI_LUA_MEM` 4 MB, `PI_LUA_INSTR` 5,000,000 instructions, `PI_LUA_MS` 10 s, per call; ext_lua.h), and one entry in `pi_exts`
(ext.c), the single ordered list: hook commands, Lua extensions in load order, then the permission gate (so the gate decides on
the arguments an extension produced). pi's order is the load order too (runner.ts iterates `this.extensions`). The order is
kept in `pi_ext_add` and nowhere else (L8): each entry names its rank (`PI_EXT_HOOKS`, `PI_EXT_LUA`, `PI_EXT_GATE`), an entry with
no rank goes to the end, so `pi_rt_open` registers the gate at its own pace and a Lua tool_call block still comes first.

The event an extension is handed is built once, for every extension kind, by `ext_payload.c` (pi's types.ts payloads as JSON);
`ext_lua.c` turns it into a table (`json.decode` semantics) and the handler's returned table back into JSON for the decision
functions that are also the hook commands' (`pi_ext_decide_call/result/input`: runner.ts emitToolCall, emitToolResult,
emitInput). `tool_call` handlers share one event; what they changed in `event.input` (compared with a second decode of the
arguments) is written back, key order of a changed object being ext_lua.c's fixed order. Handlers run on a snapshot of the
list (runner.ts snapshotEventHandlers), so `pi.on`'s unsubscribe function cannot disturb a dispatch under way.

Errors: a file that does not load is told as pi's main.ts:802 and left out; a handler that throws is told as pi's interactive
showExtensionError and its extension is called again by the next event (pi 1.1.0: runner.ts emitError only reports; L14); the loop goes on.
The tool_call case blocks with the error's message and tells nobody else (runner.ts emitToolCall has no catch; agent-session.ts:679). Differences from pi 1.1.0 and what is open are the three Lua rows of COMPATIBILITY.md
("extensions"): `ctx` is cwd, hasUI, ui (the four dialogs, next section but one) and isProjectTrusted only, the rest of `pi.*` (exec, sendMessage)
comes later; registerTool, registerCommand and appendEntry are the next section. Proof against pi: the ten L1/L2 golden scenarios of pi 1.1.0 rerun with the same
behaviour as a Lua extension (`tests/pi/rpc/ext_lua_*.cfg.json`, made by `tools/pi_oracle/gen_lua_scenarios.py`; the event
recorders compare payloads with sorted keys on both sides), the block scenario doubling as the L9-shaped reachability test (the
sentinel `pi_lua_calls`). Size: the Amiga binary grew from 1,344,904 to 1,520,672 bytes (+175,768) with Lua linked; not run on the
rig yet (L10).

## Tools, commands and entries from Lua (Phase L4, L6)

`pi.registerTool(def)` fills a `pi_lua_tool` (ext_lua.h; a `pi_tool` of agent.h plus the strings it points to) and keeps the
`execute` function in the extension's registry table `pi.tools`; the tool is added to `s->tools` once the file has loaded (a file
that fails after registering leaves nothing behind) and when `tool_allowed` says so: app.c gives it `pi_tools_ext_allowed` over
`--tools` / `--no-tools` (tools_pi.c, sdk.ts:282-294), so an extension's tool is filtered exactly as pi filters custom tools, and the
same glob (`pi_tool_glob`, createToolNameMatcher) selects built-in tools by pattern and serves MCP's `toolExposure`. A call is
`lt_run`: the arguments checked by claude/schema.c (as every other tool), `execute(toolCallId, params, nil, onUpdate, ctx)` under the
extension's limits, the returned table as content / details / structuredContent / isError (`s->result_error` marks a returned error
so the loop does not give it a thrown error's `details {}`), a raised error as the tool's error result. The model sees the tool
because the request builders read `s->tools`; nothing else knows it is Lua.

`pi.registerCommand` is kept in `pi_exts` (ext.h): `pi_exts_add_command` per owner, `pi_exts_commands` resolves the invocation names
in list order (runner.ts resolveRegisteredCommands), `pi_exts_run_command` is `_tryExecuteExtensionCommand`, called first in
`pi_prompt` (agent.c) so the line never reaches the input event. RPC `get_commands` and the slash menu (`pi_ui_ext_menu`, slot 1 of
the extra commands) read the same resolved list. `pi.appendEntry` calls `s->append_entry` (store.c `append_custom`: session3.c
`s3_custom`, then `entry_appended`); ctx_build leaves custom entries out of every request.

Proof: `tools/pi_oracle/rpc_gen.py` runs the same tools, commands and entries as TypeScript extensions in pi 1.1.0 (ext_tool*,
ext_cmd*, ext_entry; the records, `<name>.req.N.json` with the tools and messages of each request, what execute and the handlers were
handed, the session file), `gen_lua_scenarios.py` holds the Lua twins, `tests/test_pi_modes.c` compares; the reachability test is
ext_lua_entry (the fake model calls the Lua tool, sentinel `pi_lua_tool_calls`, the result is in the session file).

## Dialogs from Lua: ctx.ui (Phase L5)

pi's `ExtensionUIContext` (core/extensions/types.ts:149-160) is a different object in every mode: the TUI draws selectors, RPC sends
`extension_ui_request` lines, print and json mode use the no-op context (runner.ts:324) whose answers are `undefined` / `false`.
amiga-pi keeps that split in one place: `pi_exts.ui` (ext.h `pi_ext_ui`: select, confirm, input, notify), bound by the mode that has a
user -- `ui_pi.c` (`xu_*`, the TUI; line mode as numbered and y/n lines) and `mode_rpc.c` (`rpc_ui_*`, bound for the run of
`pi_mode_rpc`) -- and left 0 in print and json mode. Lua's `ctx.hasUI` is `ui != 0`, read when it is asked, and `ctx.ui.select`,
`confirm`, `input`, `notify` (ext_lua.c) call through it, giving pi's defaults when it is 0. pi's dialogs are Promises; a Lua handler
is synchronous, so a call returns with the answer and the mode's own loop runs while it waits: the TUI's menu loop (the same way the
permission gate's question waits: `tui_menu`), RPC's line reader (`dialog_wait`, commands that arrive meanwhile are handled, as
pi handles them). The wait is taken off the handler's clock limit (ext_lua.c `ui_waited`), so a user who thinks for a minute does not
end the extension. The TUI look is claude/tui.c's `tui_ext_menu` and `tui_ext_input`, the existing menu and line input with pi's
ExtensionSelectorComponent / ExtensionInputComponent styling (`m_ext`, `pi_in_ext`) and the dialog timeout (`m_timeout_ms`, the title
counting seconds down as pi's CountdownTimer); no second widget. Proof: RPC transcripts of pi 1.1.0 with TypeScript twins (rpc_gen.py
ui_dialogs, ui_cancel, ext_ui_timeout, ext_ui_notify, ext_ui_print) against the Lua extensions of gen_lua_scenarios.py
(`ext_lua_ui_*`); 12 TUI screens (tui_gen.py `ext_ui_*`) compared cell by cell in tests/test_pi_tui.c; tests/test_pi_lua.c for the
defaults, the arguments and the clock.

## Hook commands on pi's events (Phase L1, L2)

pi 1.1.0 has no hook commands (extensions are TypeScript modules), so the commands are amiga-pi's way to pi's extension events;
the parity target is the events' payloads (core/extensions/types.ts) and the handlers' results (core/extensions/runner.ts), not a
configuration format. Configuration: `hooks` in `ENVARC:amiga-pi/settings.json`, C:Claude's hook format (claude/config.c) with pi's
event names as keys; user settings only (hook commands in a project's files are not read; the trust-gated discovery of L3 is for Lua extensions). Runner: vtcon `claude/hooks.c`
`hooks_run_raw` (spawn `<cmd> < <event file>` through sys->run, temp file, timeout, output back); `ext_hooks.c` adds the payloads and
the pi decision semantics, nothing else. Dispatch: one ordered list, `pi_exts` (ext.c) -- the hook commands come first, the permission
gate (ext_gate.c) after them, so the gate decides on the arguments a hook produced; L8 makes the gate a member of the same list.

| pi event | payload (types.ts) | decision (runner.ts / agent-session.ts) | amiga-pi call site |
|---|---|---|---|
| `session_start` | `{reason, previousSessionFile?}` (agent-session.ts:3279, agent-session-runtime.ts) | none | app.c:841 (startup), store.c:978 (new), store.c:1019 (resume) via `pi_exts_session_start` |
| `input` | `{text, source, images?, streamingBehavior?}` (agent-session.ts:1927) | `{action:"continue"}`, `{action:"transform", text}`, `{action:"handled"}` (runner.ts:1520 emitInput; first handled wins, transforms chain) | agent.c:1125 `pi_prompt` -> `pi_exts_input`, before templates expand |
| `agent_start`, `agent_end`, `agent_settled` | `{}`, `{messages}`, `{aborted}` | none | agent.c:938, agent.c:817, agent.c:1108 (emitter) |
| `turn_start` | `{turnIndex, timestamp}` | none | agent.c:939, 962 (emitter) |
| `message_start`, `message_end` | `{message}` | none (pi: `message_end` may replace the message, not read) | agent.c:219 `emit_msg` (emitter) |
| `tool_execution_start`, `_update`, `_end` | `{toolCallId, toolName, args}`, `+ partialResult`, `{toolCallId, toolName, result, isError, durationMs?}` | none | agent.c:586, tools_pi.c:796, agent.c:638 (emitter) |
| `tool_call` | `{toolName, toolCallId, input}` (agent-session.ts:668) | `{block:true, reason?}`: the first blocker wins, the reason or "Tool execution was blocked" is the error result's text (runner.ts:1242, agent-loop.ts:746); a throwing handler blocks with its message (agent-session.ts:679); mutation of `event.input` is `{"input":{...}}` here | agent.c:610 `pi_exts_tool_call` |
| `tool_result` | `{toolName, toolCallId, input, content, details?, structuredContent?, isError}` (agent-session.ts:690) | each of `content`, `details`, `structuredContent`, `isError` replaces when given; content without structuredContent drops the structured content; later handlers see earlier changes (runner.ts:1183) | agent.c:635 `pi_exts_tool_result` |

The emitter events go to `hk_event` (ext_hooks.c) through the listener every session has (`pi_exts_event`); the payloads of the
agent events are mode_json.c's records (the same shapes, compared with pi in H) except `turn_start` (adds turnIndex and timestamp)
and `agent_end` (no willRetry). The decision events have their own call sites in the loop (the table), not the emitter, because they
return a result. Goldens: `tools/pi_oracle/rpc_gen.py` runs pi 1.1.0 with a TypeScript extension that writes each event it is handed
to `dump.jsonl` (scenarios `ext_events`, `ext_rpc_prompt`, `ext_rpc_new`) and with extensions that return the decisions
(`ext_block*`, `ext_mutate`, `ext_matcher`, `ext_result`, `ext_input_*`, `ext_rpc_handled`); `tests/test_pi_modes.c` (group
`hooks_pi`) runs amiga-pi on the same answers with hook commands that do the same, and compares pi's records and the event
files with the masks both sides share (tests/pi/rpc/masks.json: timestamps, durations, ids, the cwd).

Differences from pi 1.1.0, on purpose or not yet closed (COMPATIBILITY.md, "extension events"):
- the hook configuration and the answer format are amiga-pi's; `tool_call` mutation is a whole-object replacement;
- not hooked: `message_update` (the cumulative partial message and `partial` need pi-ai's streaming parser), `turn_end`
  (BoundaryState and the context preview), the other session events, the model events, `context`, `before_agent_start`, the
  provider events -- later items (L7);
- `input` carries no `images`;
- in RPC mode a replaced session emits `session_start` twice, as pi 1.1.0 does (rpc-mode.ts binds it in the runtime's
  finishSessionReplacement and again in the command);
- `usage` and `parentToolCallId` of the tool events, `terminate` of a blocked call, and `message_end`'s replacement message are not read.

## What is not in phase A

No product code existed at the end of phase A: no `llm/` (now in vtcon, phase B), no `amiga-pi/` sources, no program builds. Phase A delivers this document, COMPATIBILITY.md, the addendum, the layering check, the Lua probe and the ledger (`progress.md`).
