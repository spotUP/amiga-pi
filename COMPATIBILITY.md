# COMPATIBILITY: pi concept -> Amiga implementation -> status

Sources: pi at commit f10993bc (see `thoughts/shared/research/2026-10-07_pi-sources-addendum.md`) and the research R2. Status: planned / partial / done / not planned + reason. "Phase item" is a plan item id where the plan has one (B1-B12, D1-D8, A-series) and a phase letter otherwise (C, E ... O); item ids are assigned when the phase is detailed. Nothing is done yet: phase A delivers documents and probes only.

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
| Parallel tool calls | run sequentially, single-threaded | partial: sequential by design | D |

## pi-coding-agent

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Default system prompt (named sections, under about 1000 tokens) | amiga-pi/prompt.c: pi's preamble ("inside amiga-pi"), tools, rules, cwd, plus an amiga section; pi's docs section left out (amiga-pi ships no docs yet); project context and skills in G, I | partial (C) | C, G |
| Tools read/write/edit/grep/find/ls | amiga-pi/tools_pi.c over claude/tools.c | planned | D1-D5 |
| Truncation 2000 lines / 50 KB with temp file | one shared helper, T:amiga-pi-<id>.txt | planned | D5 |
| Project trust (trust.json) | reuse claude/trust.c | planned | G |
| Context files AGENTS.md / CLAUDE.md (+override), SYSTEM.md, APPEND_SYSTEM.md | agent dir then ancestors | planned | G |
| Permissions: none by default; gate as extension | claude/policy.c as built-in extension, default off | planned | D6 |
| Slash commands (/model /tree /fork /clone /compact /settings /reload /export) | slash table in amiga-pi | planned | C, E, F, H |
| /export HTML | - | not planned: no HTML exporter worth the heap; JSONL is the export | - |
| /share | - | not planned: uploads a session to a pi service | - |
| ! and !! shell escapes | user_bash through the bash tool | planned | D |
| CLI flags (-p, --mode, --model, --tools, --gate, --no-gate) | amiga-pi/cli | planned | H, D4, D6 |

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
| read {path, offset?, limit?} | claude/tools.c Read | planned | D1 |
| write {path, content} | claude/tools.c Write | planned | D1 |
| edit {path, edits[{oldText,newText}]} | new multi-edit on tools.c Edit | planned | D2 |
| bash {command, timeout?} -> runs vsh -c, AmigaDOS Shell fallback | sys->run | planned | D3 |
| grep | claude/tools.c Grep, off by default | planned | D4 |
| find | claude/tools.c Glob, off by default | planned | D4 |
| ls | directory lister, off by default | planned | D4 |

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
