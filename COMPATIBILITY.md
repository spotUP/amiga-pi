# COMPATIBILITY: pi concept -> Amiga implementation -> status

Sources: pi at commit f10993bc (see `thoughts/shared/research/2026-10-07_pi-sources-addendum.md`) and the research R2. Status: planned / partial / done / not planned + reason. "Phase item" is a plan item id where the plan has one (B1-B12, D1-D8, A-series) and a phase letter otherwise (C, E ... O); item ids are assigned when the phase is detailed. Phase A delivered documents and probes; later phases mark their rows done.

Decision D3: the pi `bash` tool is named `bash` and runs `vsh -c` (AmigaDOS Shell fallback). `shell` in older plan text is superseded.

Rows per group (124 total): pi-ai 23; pi-agent-core 6; pi-coding-agent 11; pi-tui 6; sessions 19; tools 7; extensions 20; skills 4; prompt templates 2; themes 3; RPC 12; JSON mode 1; SDK 1; MCP 9.


## pi-ai

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| API: anthropic-messages | llm/be_anthropic.c from claude/http.c, stream.c | partial (exists in C:Claude) | B5 |
| API: openai-completions | llm/be_openai_chat.c with compat flags | planned | B6 |
| API: openai-responses, azure-openai-responses | Responses adapter | planned | M |
| API: google-generative-ai | Gemini adapter, thoughtSignature replay | planned | M |
| API: bedrock-converse-stream (SigV4 + bearer key) | Converse stream adapter, own SigV4 | planned | M |
| API: google-vertex, mistral, openai-codex, pi-messages | not targeted by name; openai-compatible hosts cover Mistral | not planned: no phase; revisit after M | - |
| Content blocks text/thinking/image/toolCall | llm/llm.h, plus raw{} for unmodelled blocks | planned | B1 |
| Messages system/user/assistant/toolResult | llm/llm.h, JSON text + small struct | planned | B1 |
| Usage and cost | llm/llm.h usage, cost from model descriptor | planned | B1, B11 |
| Stream events | llm/event.h | planned | B2 |
| Event accumulator (interleaved contentIndex) | llm/accum.c | planned | B2 |
| Model descriptor | struct from models.json | planned | B, F |
| Thinking levels off..max, thinkingLevelMap | clamped per model, mapped to backend field | planned | F |
| models.json (providers, modelOverrides, $VAR / literal / !command values) | ENVARC:amiga-pi/models.json via claude/json.c | planned | B, F |
| Compat flags (maxTokensField, supportsDeveloperRole, supportsUsageInStreaming, requiresToolResultName, ...) | 4 flags first, plus detection order default->override (addendum 1) | partial: 4 of the flags | B6 |
| Credentials order: --api-key > auth.json > models.json > env | ENVARC:amiga-pi/auth.json (api_key type) | planned | F |
| Cross-provider handoff (thinking to text, id normalisation, 'No result provided') | llm transform step (addendum 2) | planned | F |
| Retry (3, 2000 ms, 60000 ms cap, Retry-After), idle timeout 300 s | llm/request.c | planned | B3 |
| Abort (stopReason aborted, partial kept) | Ctrl+C into llm/request.c | planned | B3 |
| LAN rule for http:// | llm/lan.c | planned | B8 |
| Image and classifier APIs | - | not planned: out of scope for a coding agent on 68k | - |
| Live model catalog update | static models.json only | not planned: no registry service | - |
| Websocket transport | - | not planned: no websocket client in the core | - |

## pi-agent-core

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Agent loop (turn, tool round, stop) | amiga-pi/agent.h pi_prompt() | planned | C |
| Agent events (agent_start/end/settled, turn_*, message_*, tool_execution_*) | one emitter (amiga-pi/events.c), pi's order (golden tests/pi_agent/events_two_rounds.txt); message_update carries the stream event, not a rebuilt partial message; no agent_settled yet | partial (C) | C, H |
| Steering queue (delivered after current tool calls) | queue checked between tool calls | planned | C |
| Follow-up queue (delivered when agent would stop) | queue checked at stop | planned | C |
| Abort | Ctrl+C | planned | C |
| Parallel tool calls | run sequentially, single-threaded (agent.c, one call after the other in message order) | partial: sequential by design | D |

## pi-coding-agent

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Default system prompt (named sections, under about 1000 tokens) | amiga-pi/prompt.c: pi's preamble ("inside amiga-pi"), tools, rules, cwd, plus an amiga section; pi's docs section left out (amiga-pi ships no docs yet); project context and skills in G, I | partial (C) | C, G |
| Tools read/write/edit/grep/find/ls | amiga-pi/tools_pi.c over claude/tools.c (tools_run on one cl_tools with pi_rules); defaults read, bash, edit, write as pi; each tool's promptGuidelines in the rules | done (D) | D1-D5 |
| Truncation 2000 lines / 50 KB with temp file | amiga-pi/truncate.c: bash keeps the tail of its output (the whole of it in T:amiga-pi-<call id>.txt, pi's notice) and its status after the notice; read keeps the head with pi's offset notice; grep, find and ls cut at 50 KB only (pi_head) with pi's notices joined in one [...] | done (D, parity) | D5 |
| Project trust (trust.json) | reuse claude/trust.c | planned | G |
| Context files AGENTS.md / CLAUDE.md (+override), SYSTEM.md, APPEND_SYSTEM.md | agent dir then ancestors | planned | G |
| Permissions: none by default; gate as extension | amiga-pi/ext_gate.c on tool_call: claude/config.c rules (cfg_decide, deny > ask > allow) and C:Claude's ask rule (perm_must_ask), the question ui_ask; permissionGate in ENVARC:amiga-pi/settings.json, --gate / --no-gate; print mode refuses what would ask; "yes, always in this project" keeps C:Claude's rule for the call (claude/config.c cfg_rule_for) in <data dir>/permissions/--<cwd>--.json (the data-directory rule: UP-Term:var/amiga-pi, else ENVARC:amiga-pi; the name as pi names its session directories), read back at the next start | done (D, parity) | D6 |
| Slash commands (/model /tree /fork /clone /compact /settings /reload /export) | slash table in amiga-pi | planned | C, E, F, H |
| /export HTML | - | not planned: no HTML exporter worth the heap; JSONL is the export | - |
| /share | - | not planned: uploads a session to a pi service | - |
| ! and !! shell escapes | user_bash through the bash tool | planned | D |
| CLI flags (-p, --mode, --model, --tools, --gate, --no-gate) | amiga-pi/args.c: -p, --model, --base-url, --tools/-t (list, +name, -name, *), --no-tools/-nt, --gate, --no-gate; --mode in H | partial (D) | H, D4, D6 |

## pi-tui

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Differential rendering | claude/tui.c (existing) until N | partial | N |
| Keybindings (keybindings.json, action ids) | claude/keys.c + JSON map (addendum 8) | planned | N |
| Editor (cursor, kill ring, history) | claude/input.c, edit.c, hist.c | partial (exists) | N |
| Message queues in the UI (Enter steers, Alt+Enter follows up) | input handler to the two queues | planned | N |
| Model cycle (Ctrl+P), thinking cycle (Shift+Tab) | keybindings | planned | F, N |
| Themes (system/dark/light, JSON vars+colors) | claude/theme.c extended to pi's JSON (addendum 8) | planned | N |

## sessions

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| v3 header line | llm/session3.h writer/reader | planned | E |
| Entry: message | session3 | planned | E |
| Entry: model_change | session3 | planned | E, F |
| Entry: thinking_level_change | session3 | planned | E, F |
| Entry: usage | session3 | planned | E |
| Entry: compaction | session3 + summariser | planned | E |
| Entry: context_edit | session3 | planned | E |
| Entry: branch_summary | session3 | planned | E |
| Entry: custom (not in context) | session3 (extension data) | planned | E, L |
| Entry: custom_message (in context as user) | session3 | planned | E, L |
| Entry: label | session3 | planned | E |
| Entry: session_info | session3 | planned | E |
| Tree (leaf, parentId, context = leaf to root) | RAM index, branch loaded | planned | E |
| /tree (move leaf, optional LLM branch summary) | slash command | planned | E |
| /fork (new file, parentSession) | slash command | planned | E |
| /clone (copy branch) | slash command | planned | E |
| Compaction (reserve 16384, keep 20000, cut rules, split-turn) | amiga-pi/compact.c, prompts copied from pi (addendum 6) | planned | E |
| Branch summary (BRANCH_SUMMARY_PROMPT) | same | planned | E |
| Session file location and slug | ENVARC:amiga-pi/sessions/--<slug>--/ | planned | E |

## tools

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| read {path, offset?, limit?} | amiga-pi/tools_pi.c run_read as pi's read.ts: the text as it is (no line numbers), split at LF, offset/limit, 2000 lines / 50 KB with pi's offset notices and errors (ENOENT, "Offset N is beyond end of file", the over-long first line's sed hint), no temp file; images by their first bytes (pi's detectSupportedImageMimeType: PNG not animated, JPEG, GIF, WebP, BMP) through amiga-pi/image.c as pi's processImage: BMP to PNG, over 2000x2000 or 4.5 MB of base64 scaled (Lanczos3, EXIF orientation first) and sent as PNG with pi's notes, 3/4 steps until it fits (decoders and PNG writer: stb_image/stb_image_write, vendor/stb; +60 KB on the Amiga). Differs: a file that is not UTF-8 is read as Latin-1 (pi: UTF-8 with U+FFFD); a file over 16 MB is refused (memory); pi also tries JPEG at falling qualities (amiga-pi writes PNG only, so it may scale further) and decodes WebP (amiga-pi passes a WebP through when it fits, else omits it); the non-vision-model note waits for model metadata (F) | done (D, parity) | D1 |
| write {path, content} | claude/tools.c Write under pi_rules: no read first, the directories made; C:Claude's result words | done (D) | D1 |
| edit {path, edits[{oldText,newText}]} | claude/edit_orig.c tools_edit_orig on Edit's loading and write, as pi's edit-diff.ts: BOM kept apart, CRLF matched as LF and restored, exact match first, then pi's fuzzy normalisation (NFKC by claude/unorm.c, Unicode 16.0, white space at line ends, smart quotes, dashes, special spaces) with the untouched lines kept, occurrences counted in the normalised text; against the original, unique, not overlapping, all or none, pi's error words, pi's legacy argument forms; pi's own fuzzy and CRLF/BOM test cases. Deliberate difference: a file that is not UTF-8 is read and written back as Latin-1 (C:Claude's rule; pi would decode it as UTF-8 and write U+FFFD back). Files over 1 MB are refused (Edit's limit) | done (D, parity) | D2 |
| bash {command, timeout?} -> runs vsh -c, AmigaDOS Shell fallback | claude/tools.c Bash under pi_rules through sys->run (sys_amiga: `vsh <script>` when vsh is installed, else Execute in the AmigaShell); starts in the working directory each time, no default time limit, a time limit stops it; pi's status words after the output; the prompt says which shell | done (D) | D3 |
| grep | claude/search_pi.c tools_pi_grep as pi's rg --json --line-number --hidden [--ignore-case] [--fixed-strings] [--glob]: claude/ignore.c honours .gitignore (inside a git repository), .ignore, .rgignore and .git/info/exclude as rg does; "path:N: text", context "path-N- text", 500-character lines, limit 100, 50 KB, pi's notices; checked against rg on random trees (vtcon tests/test_pi_search.py). Differs: name order, depth first (rg's order is unspecified); matching ignores case in file names (AmigaDOS); a file over 1 MB is not searched (memory); no global gitignore (core.excludesFile) | done (D, parity) | D4 |
| find | claude/search_pi.c tools_pi_find as pi's fd --glob --hidden (--no-require-git outside a repository; a pattern with '/' matches the full path from any depth): .gitignore, .ignore, .fdignore, .git/info/exclude, every .git left out; paths relative to the search directory, '/' after directories, limit 1000, 50 KB, pi's notices, fd's glob errors; checked against rg's ignore rules and git (tests/test_pi_search.py). Differs: name order (fd's is unspecified); names match ignoring case | done (D, parity) | D4 |
| ls | amiga-pi/tools_pi.c over sys->list, as pi's ls.ts: sorted as toLowerCase().localeCompare (ICU, English: amiga-pi/collate_tab.h, generated with node), '/' after directories, limit 500, 50 KB, pi's notices and errors | done (D, parity) | D4 |

## extensions

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Loading (TS via jiti in pi) | Lua 5.4 script loaded by the embedded interpreter (decision D4) | planned (different language) | L |
| Event group: session_* | ext.h table | planned | L |
| Event group: agent/turn/message | ext.h table | planned | L |
| Event group: tool_call (block, mutate), tool_result (replace) | amiga-pi/ext.c for built-in C extensions (C3); hooks and Lua in L; carries the gate | partial (C) | L, D6 |
| Event group: input, user_bash | ext.h table | planned | L |
| Event group: context, before_agent_start | ext.h table | planned | L |
| Event group: model_select, thinking_level_select, provider_stream_event | ext.h table | planned | L |
| Event group: project_trust, resources_discover, mcp_servers_change | ext.h table | planned | L |
| registerTool | function table entry | planned | L |
| registerCommand | function table entry | planned | L |
| registerShortcut, registerFlag | function table entry | planned | L |
| registerProvider | function table entry (backend registration) | planned | L, M |
| registerMcpServer | function table entry | planned | L, K |
| registerToolRenderer, registerEntryRenderer | - | not planned: renderers are TS closures over pi-tui components | - |
| pi.events bus | small in-process bus | planned | L |
| Actions sendMessage / sendUserMessage / appendEntry / setLabel / exec / get-set model | function table entries | planned | L |
| ctx.ui calls (notify, confirm, select, input, status, widgets) | text-mode implementations over the TUI | partial: text prompts only | L |
| Hook commands (claude/hooks.c mechanism) | first extension mechanism | planned | L |
| Packages (npm, git) | - | not planned: npm packages need a JS runtime | - |
| codemode (QuickJS tool scripts) | - | not planned: needs a JS runtime | - |

## skills

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| SKILL.md frontmatter (name, description, license, compatibility, metadata, allowed-tools, disable-model-invocation) | parser over claude/json.c style line parser | planned | I |
| Discovery roots (agent dir, .pi/skills, .agents/skills, ancestors) | directory walk | planned | I |
| <available_skills> section, load via read | prompt section | planned | I |
| /skill:name args | slash command | planned | I |

## prompt templates

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| prompts/*.md as /name, frontmatter description and argument-hint | loader | planned | J |
| Argument substitution $1 $@ $ARGUMENTS ${1:-d} ${@:N} ${@:N:L}, shell-like quoting | one pure function | planned | J |

## themes

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Theme JSON (appearance, vars, colors) | theme loader | planned | N |
| Built-in system / dark / light | built-in tables; system uses terminal palette query | partial | N |
| Project themes (.pi/themes, trust-gated) | trust check | planned | N, G |

## RPC

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| prompt, steer, follow_up, abort, clear_queue | JSONL on stdin/stdout | planned | H |
| new_session, switch_session, fork, clone, get_fork_messages | same | planned | H |
| get_state, get_messages, get_entries, get_tree, get_last_assistant_text, get_session_stats | same | planned | H |
| set_model, cycle_model, get_available_models | same | planned | H |
| set_thinking_level, cycle_thinking_level | same | planned | H |
| set_steering_mode, set_follow_up_mode | same | planned | H |
| compact, set_auto_compaction | same | planned | H |
| set_auto_retry, abort_retry | same | planned | H |
| bash, abort_bash | tool named bash per D3 | planned | H |
| export_html | - | not planned: see /export | - |
| set_session_name, get_commands | same | planned | H |
| Extension UI sub-protocol (extension_ui_request / response) | same JSONL | planned | H, L |

## JSON mode

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Events: session header, agent_*, turn_*, message_*, tool_execution_*, queue_update, compaction_*, auto_retry_* | one event sink | planned | H |

## SDK

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| createAgentSession / prompt / steer / followUp / abort / subscribe | C header amiga-pi/sdk.h over pi_prompt | partial: C API, not TS | H |

## MCP

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Transport stdio | needs process pipes (ixpipe-handler / vsh) | planned | K |
| Transport streamable HTTP | over llm HTTP stack | planned | K |
| Legacy SSE transport | - | not planned: pi itself rejects it | - |
| Exposure direct | tools named mcp__server__tool | planned | K |
| Exposure deferred (tool_search), hidden | tool_search tool | planned | K |
| Exposure codemode (default in pi) | - | not planned: needs QuickJS; amiga-pi default is direct | - |
| mcp.json config | JSON loader | planned | K |
| OAuth with dynamic client registration | - | not planned until probe O1 shows feasibility | O |
| Result truncation over 20 KB to temp file | shared truncation helper | planned | K |
