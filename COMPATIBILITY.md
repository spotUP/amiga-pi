# COMPATIBILITY: pi concept -> Amiga implementation -> status

Sources: pi at commit f10993bc (see `thoughts/shared/research/2026-10-07_pi-sources-addendum.md`) and the research R2. Status: planned / partial / done / not planned + reason. "Phase item" is a plan item id where the plan has one (B1-B12, D1-D8, A-series) and a phase letter otherwise (C, E ... O); item ids are assigned when the phase is detailed. Phase A delivered documents and probes; later phases mark their rows done.

Decision D3: the pi `bash` tool is named `bash` and runs `vsh -c` (AmigaDOS Shell fallback). `shell` in older plan text is superseded.

Rows per group (174 total): pi-ai 31; pi-agent-core 6; pi-coding-agent 13; pi-tui 20; sessions 26; tools 7; extensions 20; skills 4; prompt templates 2; themes 3; RPC 12; JSON mode 1; SDK 1; MCP 28.


## pi-ai

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| API: anthropic-messages | llm/be_anthropic.c from claude/http.c, stream.c | partial (exists in C:Claude) | B5 |
| API: openai-completions | llm/be_openai_chat.c with compat flags | planned | B6 |
| API: openai-responses | llm/be_openai_responses.c, pi-ai's api/openai-responses.js ported; body, SSE, handoff, session id (prompt_cache_key, session_id headers) byte-tested against pi (vtcon tests/pi/responses, switch goldens); gpt-5.5 and gpt-4.1 use it, reachable through pi_app (llm_feeds_responses) | done (FA4) | FA4 |
| API: azure-openai-responses | Responses adapter with Azure's URL and api-version | planned | M |
| Model switch and settings (FA6) | A switch uses settings.json defaultThinkingLevel before the session's level (pi's _getThinkingLevelForModelSwitch) and leaves the session's model and level unchanged when the session file cannot take the entries. pi's `/model <term>` calls setModel with persist:false (interactive-mode.js:4301), so amiga-pi's /model never writes defaultProvider/defaultModel either; pi persists them only from the selector's set-default action and after /login (:4429, :5063), which amiga-pi has neither of | done | FA6 |
| Gaps in the ported providers (found by FA4/FA2, not done) | (1) models carry `cost.tiers` (gpt-5.5 above 272000 input tokens); llm_model has the base price only, so the cost of a long-context answer is under-counted. (2) the anthropic-messages body is pi's byte for byte (vtcon tests/pi/anthropic, made by pi) for the flags this port reads: supportsMidConvoEffort / SystemMessages / ToolChanges, supportsEagerToolInputStreaming, supportsCacheControlOnTools, allowEmptySignature, forceAdaptiveThinking. Not ported: `PI_CACHE_RETENTION=long` (the 1h cache ttl), `allowedFallbackModels` (`fallbacks` member and its beta; claude-fable-5), OAuth subscription tokens. (3) pi's per-model thinking defaults in settings (`getModelThinkingLevel`) | open gap | follow-up |
| Strict tools and the output cap (PX1, PX2) | read, bash, edit and write declare pi's `constrainedSampling` `{type:json_schema, strict:prefer}` (grep, find, ls and MCP tools do not, as in pi); vtcon `llm/constrain.c` (one implementation for anthropic-messages, openai-responses, openai-completions) makes pi's strict schema (makeStrictJsonSchema: additionalProperties false, every property required, an optional one `anyOf [..., null]`) and sends the tool `strict:true` when the model's compat has supportsStrictTools (anthropic) / supportsStrictMode (openai); a schema outside the strict subset (including Anthropic's refused keywords) goes out plain, as pi's "prefer". Differences: the error text of a `strict:"require"` tool is not pi's reason-specific text (nothing in amiga-pi sets require). `llm/estimate.c` is pi's estimateContextTokens (3.5 characters per token in UTF-16 units, images 4800, last good answer's usage plus what follows it) and clampMaxTokensToContext: every request's `max_tokens` / `max_output_tokens` / `max_completion_tokens` is min(cap, max(1, window - estimate - 4096)); the estimate is over the transcript with its system messages (the agent), the summarization request of compaction over its prompt and user message. Goldens made by pi: vtcon tests/pi/{anthropic,responses,completions,strict,estimate}, amiga-pi tests/pi/model/{switch,clamp_first,declare}* | done (PX1, PX2) | PX |
| Tool changes declared before every request (PX3) | pi's declareToolChanges runs before each request whatever the model: after tool_search loads a tool the session file gets the system message with toolsAdded after the tool result; a model without mid-conversation system messages is sent the collapsed prompt and tool list (golden from pi-agent-core runAgentLoop: tests/pi/model/declare_*) | done (PX3) | PX |
| Store flag of openai-completions (PX4) | pi-ai's `supportsStore` (default: every host not listed as non-standard, i.e. not nvidia, cerebras, xai, together, chutes, deepseek, zai, moonshot, opencode, cloudflare, ant-ling; models.json `compat.supportsStore` overrides): the body carries `"store":false` after `stream_options`; vtcon `llm/be_openai_chat.c` (detect, compat_of, `llm_compat.store`). Goldens made by pi-ai's openai-completions (vtcon tests/pi/completions: a LAN host, a non-standard host, supportsStore false and true), the `supportsStore:false` isolation of the earlier goldens removed | done (PX4) | PX |
| Mid-conversation system messages and tool additions on openai-responses (PX5) | pi-ai's resolveTranscript / resolveTranscriptTools / convertResponsesMessages for `supportsMidConvoSystemMessages` and `supportsAdditionalTools` (gpt-5.5): a later system message stays in place as a developer item holding its update text, the tools it adds go out as an `additional_tools` item (`role:"developer"`, the tools as the request's) and the request's `tools` stays the initial list; after a removal or a redeclared tool the request lists the current tools and no `additional_tools` item is sent (pi's hasNonAdditiveToolChanges); a model without the flag gets the collapsed prompt and tool list as before. vtcon `llm/transcript.c` (one implementation with anthropic-messages: system text, update text, tool state), `llm/be_openai_responses.c`; amiga-pi's agent sends the transcript's system messages to every model with `llm_mid_system`. Goldens: vtcon tests/pi/responses sys_mid_tool, sys_mid_redef (pi-ai), amiga-pi tests/pi/model/declare55_* through pi_app (pi-agent-core runAgentLoop). Not ported: `supportsToolSearch` (tool_search_call items, no catalog model has it without supportsAdditionalTools) | done (PX5) | PX |
| Gaps found by PX (not done) | (1) amiga-pi's bash tool description names vsh (a wording difference on purpose); the other tool declarations equal pi's byte for byte | open gap | follow-up |
| API: google-generative-ai | Gemini adapter, thoughtSignature replay | planned | M |
| API: bedrock-converse-stream (SigV4 + bearer key) | Converse stream adapter, own SigV4 | planned | M |
| API: google-vertex, mistral, openai-codex, pi-messages | not targeted by name; openai-compatible hosts cover Mistral | not planned: no phase; revisit after M | - |
| Content blocks text/thinking/image/toolCall | llm/llm.h, plus raw{} for unmodelled blocks | planned | B1 |
| Messages system/user/assistant/toolResult | llm/llm.h, JSON text + small struct | planned | B1 |
| Usage and cost | llm/llm.h usage; the message's cost from the model's price (llm_acc_model), `model` is the configured id and the server's own name `responseModel` | done (F) | B1, F |
| Stream events | llm/event.h | planned | B2 |
| Event accumulator (interleaved contentIndex) | llm/accum.c | planned | B2 |
| Model descriptor | llm_model (llm/backend.h) in llm/registry.c; bundled catalog llm/models_builtin.json is hand-made (pi's generated catalog data is not in its repository): Anthropic from C:Claude's price table, gpt-5.5 / gpt-4.1 with GPT-5 / 4.1 list prices, an Ollama example; DIFFERENCE: values are not pi's catalog | done (F) | F1 |
| Thinking levels off..max, thinkingLevelMap | pi's getSupportedThinkingLevels / clampThinkingLevel (oracle-tested); anthropic-messages: budget (pi's budgets 1024/2048/8192/16384, answer room 1024) / adaptive with effort / disabled, interleaved-thinking beta header; openai-completions: reasoning_effort. DIFFERENCES: only thinkingFormat "openai" (zai, qwen, openrouter, together, deepseek, string-thinking, chat-template send no thinking fields); max_tokens is not clamped to the remaining window (clampMaxTokensToContext); no per-level samplingParams; settings thinkingBudgets ignored | done (F) | F |
| models.json (providers, modelOverrides, $VAR / literal / !command values) | llm/models_json.c over llm/registry.c: provider baseUrl/api/apiKey/headers/authHeader/compat, models (add/replace), modelOverrides, comments and BOM; a bad file is a warning and the catalog stays. `!command` runs once per process (pi's resolveConfigValue caches; its docs say per request). DIFFERENCES: inputLimits, promptCache, samplingParams*, oauth and most compat flags accepted and ignored; error text is shorter than pi's typebox list | done (F) | F2 |
| Compat flags (maxTokensField, supportsDeveloperRole, supportsUsageInStreaming, requiresToolResultName, ...) | 4 flags first, plus detection order default->override (addendum 1) | partial: 4 of the flags | B6 |
| Credentials order: --api-key > auth.json > models.json > env | llm/creds.c; auth.json api_key entries (key is a config value); env is <PROVIDER>_API_KEY only; no OAuth | done (F) | F2 |
| Cross-provider handoff (thinking to text, id normalisation, 'No result provided') | llm/handoff.c = pi's transformMessages (same model = provider + api + id; foreign thinking becomes PLAIN text, not tags; ids per api; images downgraded; 12 oracle cases) then llm_transform | done (F) | F3 |
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
| Project trust (trust.json) | `ENVARC:amiga-pi/trust.json` in pi's flat format ({"<canonical dir>": true/false}, sorted keys, 2-space JSON; bytes equal pi's ProjectTrustStore) through claude/trust.c (`trust_near`, `trust_put`); needed when `<cwd>/.pi/` holds settings.json, mcp.json, extensions, skills, prompts, themes, SYSTEM.md or APPEND_SYSTEM.md, or the directory or an ancestor holds `.agents/skills`; order: `-a`/`--approve`, `-na`/`--no-approve`, the nearest saved decision, settings.json defaultProjectTrust (ask, always, never), the question; the question is numbered (pi's five answers) and asked in typed mode only, print mode is not trusted; `/trust` lists the answers that can be kept and `/trust N` keeps one. Differences: keys are canonical AmigaDOS paths compared without case; no `~/.agents/skills` exception (no home); a Latin-1 folder name is stored and matched as UTF-8 (ASCII letters compare without case, Latin-1 letters do not); `/trust N` answers in pi's words with amiga-pi for pi ("Saved trust decision: trusted. Restart amiga-pi for this to take effect."); a trust.json that pi's readTrustFile refuses (not JSON, not an object, a value other than true/false/null) stops amiga-pi with pi's error text (`Failed to read trust store <file>: ...` / `Invalid trust store <file>: ...`), is never rewritten, and a kept answer that cannot be written is the same kind of error | done | G3, GA1, GA3, GA4 |
| Context files AGENTS.md / CLAUDE.md (+override), SYSTEM.md, APPEND_SYSTEM.md | `amiga-pi/resources.c` over claude/memory.c's `mem_walk` with pi's candidates (AGENTS.override.md, AGENTS.md, AGENTS.MD, CLAUDE.md, CLAUDE.MD; first found per directory): the agent directory first, then the volume root down to the working directory (eight levels at most), each file once, a BOM dropped, not trust-gated (as pi); `project_context` section of `<project_instructions path>`; `.pi/SYSTEM.md` replaces the preamble (no tools, rules or amiga sections) and `.pi/APPEND_SYSTEM.md` becomes the `addendum` section, for a trusted project, else the agent directory's; the sections equal pi's for 5 layouts (tests/pi/resources, from pi's own code). Differences: pi's `docs` section is the `amiga` section (dropped under SYSTEM.md, as pi drops docs); `AGENTS.MD` and `AGENTS.md` are one name on FFS; no git worktree shadowing; no `--system-prompt` flag yet; pi walks every level up, here eight at most | done | G1, G2 |
| Context files: size limit and unreadable files | a context file over 256 KB (`PI_RES_MAX`) is passed over, as an unreadable one is (pi passes over a file it cannot read); an unreadable `.pi/SYSTEM.md` or `.pi/APPEND_SYSTEM.md` falls back to the agent directory's file, then to the default. Differences: pi has no size limit; the limit protects the 68k's memory | done | G1, GA |
| Context files: same file by canonical name | the agent directory (`ENVARC:amiga-pi`) and the working directory are compared by NameFromLock names, so a working directory inside or equal to the agent directory loads that AGENTS.md once | done | GA6 |
| Permissions: none by default; gate as extension | amiga-pi/ext_gate.c on tool_call: claude/config.c rules (cfg_decide, deny > ask > allow) and C:Claude's ask rule (perm_must_ask), the question ui_ask; permissionGate in ENVARC:amiga-pi/settings.json, --gate / --no-gate; print mode refuses what would ask; "yes, always in this project" keeps C:Claude's rule for the call (claude/config.c cfg_rule_for) in <data dir>/permissions/--<cwd>--.json (the data-directory rule: UP-Term:var/amiga-pi, else ENVARC:amiga-pi; the name as pi names its session directories), read back at the next start | done (D, parity) | D6 |
| Slash commands (/model /thinking /tree /fork /clone /compact /settings /reload /export) | slash table in amiga-pi | planned | C, E, F, H |
| /export HTML | - | not planned: no HTML exporter worth the heap; JSONL is the export | - |
| /share | - | not planned: uploads a session to a pi service | - |
| ! and !! shell escapes | user_bash through the bash tool | planned | D |
| CLI flags (-p, --mode, --model, --tools, --gate, --no-gate) | amiga-pi/args.c: -p, --model, --base-url, --tools/-t (list, +name, -name, *), --no-tools/-nt, --gate, --no-gate; --mode in H | partial (D) | H, D4, D6 |

## pi-tui

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Differential rendering | claude/tui.c's diffed footer rows with TUI_LOOK_PI (`t->look`): the editor between two rules, the working row in the top rule, the queue rows, the slash list, the footer (directory, usage, model). The transcript scrolls in the terminal's scroll region (pi keeps its own scrollback). A typed key costs the changed rows: 6 keys sent 327 bytes against C:Claude's look 902 (tests/test_pi_tui.c test_footer_cost) | done (N0) | N0, N7 |
| Look parity: the captures | tests/pi/tui/*.golden are pi 1.0.4 in tmux at 80x25 (`capture-pane -e`, tools/pi_oracle/tui_gen.py, a fake Messages API, default `system` theme, TERM=xterm-256color): start, editor, slash menu, markdown (two documents and a table), tool block, failed tool, request error, working row, queued messages, a second prompt, /tree, the trust question, `!` commands. tests/test_pi_tui.c compares text, colours and attributes cell by cell; every cell that differs on purpose is a mask with its reason, listed by the rows below | done (N0, N7) | N0, N7 |
| Differences: start header | The logo and the key hints are pi's; the line names `amiga-pi (pi 1.0.4)` instead of `v1.0.4`; the "Press ctrl+o to show full startup help and loaded resources." line, pi's tip about asking it for its docs and the loaded-resources listing are not shown (no bundled docs; Ctrl+O is the transcript viewer). `quietStartup: true` hides the header as in pi | done (N0) | N0 |
| Differences: slash menu | The list holds the commands amiga-pi has (/model /tree /thinking /export /name /session /fork /clone /trust /quit), not pi's 24. The layout is pi's SelectList (five rows around the selection, `(n/total)`), tested with pi's whole list | done (N0) | N0 |
| Differences: colours | pi's default `system` theme with no terminal colours is ANSI indices 0-15 with neutral roles faint: byte for byte the same here (oracle tests/pi/tui/system_theme.txt). dark.json, light.json and a user's theme use hex, okhsl() and oklch(): brought to the screen's 16 ANSI colours by view/hl_style.c's hl_rgb_to_16 in claude/theme.c (pitheme_sgr, the one place); pi's logo names three 256-palette colours and sends them as they are. No truecolour path | done (N0, N6) | N0, N6 |
| Differences: code in answers | Fences, indent and the colours of the roles are pi's; the tokens come from view/hl_lex, not highlight.js: C's `int` and `void` are keywords as in pi, but pi also colours a function's parameter list (`(void)`) as a variable and this does not; other languages differ token by token | partial | N0 |
| Differences: errors | A failed request shows `Error: ` and amiga-pi's message (llm layer: `HTTP 400: model: ...`); pi shows the SDK's raw message (`400` then the JSON body) and a "/bug" hint line, which amiga-pi does not have | partial | N0 |
| Differences: context figure | pi's getContextUsage (compact.c cp_context_usage over the session): the last answer's usage plus an estimate of what followed, else the current system message with its tools' declarations and the messages (0.7% for one short prompt in pi's capture: its system prompt and tools); `?` after a compaction until an answer with usage follows. The screen captures mask the figure while no answer has usage: those tests run a bare agent with a one-line system prompt (tests/test_pi_tui.c test_context_figure checks it through pi_app). "(auto)" is always shown; no git branch, no extension statuses, no "(provider)" prefix, no subscription mark. The cost is rounded to the even digit on an exact tie (pi: floating-point toFixed) | partial | N0 |
| Differences: working row | The spinner turns on the screen's ticks, pi's on a timer: the frame in a capture is a moment of it | done | N0 |
| Differences: the selector at the start | pi's trust question fills the top of the empty screen; here the menu is drawn in the footer area at the bottom of the window (tui_menu, no footer under it before the screen is up). The keys line is fixed text, not read from the keymap | done | N0 |
| Tool blocks | read, bash, write, edit, ls, find, grep as pi's renderers draw the call and the collapsed result (bash: the last five rows, `... (N earlier lines, ctrl+o to expand)`, `Took 0.0s`; ls 20, find 20, grep 15 lines). Not drawn: write's content preview, edit's diff, images, a call's live update in place (the header shows when the call starts, the result appended when it ends), partial bash output while it runs | partial | N0 |
| A session drawn again | A resumed session and the branch after /tree show their user messages, answers (Markdown) and tool blocks without durations. Branch summaries, compaction summaries and custom messages are not drawn as pi's components; thinking blocks are not shown and Ctrl+T (collapse them) does nothing | partial | N3 |
| Keybindings (keybindings.json, action ids) | claude/keys.c km_init_pi / km_load_pi: every action id of docs/keybindings.md (pi 1.0.4), a value or a list replacing the default, `[]` switching off, unknown ids and bad keys as warnings shown at the start. The tui.editor.* motions, tui.altScreen.*, app.suspend, app.message.copy, app.thinking.toggle and the session pickers' ids are accepted and keep what they do (the editor's own keys, nothing); `super`, function keys and wheel keys do not bind | done (N6) | N6 |
| Editor (cursor, kill ring, history) | claude/input.c, edit.c, hist.c; the cursor is drawn as pi draws it (a reverse cell), at most 30% of the rows tall, `↑ n more` in the rule when scrolled | done | N0 |
| Message queues in the UI (Enter steers, Alt+Enter follows up) | agent.c pi_steer / pi_follow_up at pi-agent-core's points (after the turn's tool calls; when the run would stop), settings steeringMode / followUpMode one-at-a-time or all, the rows `Steering: ...`, `Follow-up: ...` and the Option+Up hint, Alt+Up and Esc take them back to the editor. A `/` or `!` line typed while the agent works waits in the type-ahead queue until it ends (pi runs commands at once); typed while compacting is not queued separately | done (N1) | N1 |
| Model cycle (Ctrl+P), thinking cycle (Shift+Tab) | models.c pi_model_cycle / pi_thinking_cycle: the models of settings enabledModels (globs over provider/id and id, `:level`) that have a credential in the order of the patterns, else all that have one; model_change then thinking_level_change as /model writes them; pi's status lines. Not read: pi's per-model thinking levels; the status line is added to the transcript, pi updates the last one in place | done (N2) | F4, N2 |
| /tree | tree_pi.c over the session index (session3.h): pi's flatten and connectors, the active path mark, labels, fold marks, filters (default, no-tools, user-only, labeled-only, all; cycle), search by typing, fold and branch keys, label editing, "Summarize branch?" and the move (cmds.c navigate), the conversation drawn again. Not done: label timestamps (the toggle shows its word only), copying the entry, panning of long rows (they are cut); the help line names this screen's keys (`alt` where pi's capture says `option`) | done (N3) | N3 |
| /export | export.c: the current branch as a standalone HTML page (`amiga-pi-session-<file>.html` in the working directory, or the path given) or as JSONL when the path ends in .jsonl (pi's serializeSessionBranch); pi's messages. The page is static; pi's is a script that renders the session in the browser (marked, highlight.js): too large here. claude/html.c turns HTML into Markdown, the other way, and is not used | partial (N4) | N4 |
| ! and !! | A line starting with `!` runs through the bash tool without the gate; the output is recorded as a bashExecution message and goes into the next request as pi's convertToLlm words it; `!!` marks it excludeFromContext. The block is drawn when the command ends (pi streams it); the rules take the bash colour while the line is typed | done (N5) | N5 |
| User message echo | The message shows when the conversation takes it (message_end), also a queued one when it is delivered, as pi's | done | N1 |

## sessions

Phase E (2026-10-11). Every row below was compared with pi's own code: `tools/pi_oracle/gen.ts` runs pi f10993bc's `SessionManager`, `buildSessionContext`, `convertToLlm`, `prepareCompaction`, `compact` and `generateBranchSummary` (with a fake model) over `tests/pi/sessions/*.jsonl` and a writer scenario, and `tests/test_pi_session.c` compares session3.c, context.c and compact.c with that output line for line.

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| v3 header line (`type`, `version`, `id` uuidv7, `timestamp`, `cwd`, `parentSession`) | amiga-pi/session3.c writer and reader; the file is made once a user or assistant message exists (pi's "wx"), then one appended line per entry | done | E1 |
| Entry: message | session3 `s3_message`; the agent loop appends every finished user, assistant and toolResult message (a `message_end` subscriber in store.c) | done | E1, E10 |
| Entry: model_change | a new session records it first (pi's sdk.ts); restored on resume | done | E1, E7 |
| Entry: thinking_level_change | recorded "off" in a new session (amiga-pi has no thinking levels yet) | done, levels F | E1, E7 |
| Entry: usage | session3 `s3_usage` (written when something reports usage; none does yet) | done (writer) | E1 |
| Entry: compaction | session3 + compact.c; carries pi's `systemMessage` (the replay), `details`, `usage`, `fromHook: false` | done | E1, E4 |
| Entry: context_edit | session3 (pi's validation: editable target on the active branch, a string becomes one text block for assistant and toolResult) + context.c (latest edit per target, null omits) | done; no command creates one yet | E1, E3 |
| Entry: branch_summary | session3 + compact.c | done | E1, E5 |
| Entry: custom (not in context) | session3 `s3_custom` | done (writer); extensions L | E1, L |
| Entry: custom_message (in context as user) | session3, context.c | done (writer, context); extensions L | E1, E3, L |
| Entry: label | session3 (resolved per target; cleared by an empty label); `/label` | done | E1, E5 |
| Entry: session_info | session3 (pi's name sanitising); `--name`, `/name` | done | E1, E7 |
| Tree (leaf, parentId, context = leaf to root) | session3 keeps an index of every entry (id, parent, type, role, file offset) and only the active branch's lines in RAM; off-branch lines are read back on demand (`cl_sys.pread`); context.c builds the context | done | E2, E3 |
| Old files | version 1 and 2 migrated and rewritten as pi does; malformed lines skipped; a file that is not a session refused and left alone; a missing final newline repaired | done | E2 |
| System prompt as a system message in the transcript | store.c: the first request writes `{"role":"system","content":"","sections":{preamble,tools,rules,amiga,cwd},"toolsAdded":[...]}`; later only a patch (changed sections, null for a removed one, toolsAdded/toolsRemoved); the request's system prompt is the replay (ctx_system_text) | done; pi's "docs" section left out (no pi docs on the Amiga), an "amiga" section added | E3b |
| /tree (move leaf, optional LLM branch summary) | amiga-pi/cmds.c: `/tree` lists the conversation (numbered, the active branch marked, labels), `/tree N` moves the leaf (a user message: its parent becomes the leaf and its text comes back to the input), `/tree N summarize [focus]`, `/tree N summarize-with PROMPT`, `-l LABEL`. pi's full-screen selector and its filter modes are not built; the list shows user and assistant messages, compactions, branch summaries and custom messages | done (commands, not a selector) | E5 |
| /fork (new file, parentSession) | cmds.c: `/fork` lists the branch's user messages, `/fork N` starts a session from just before message N (pi's createBranchedSession: labels re-chained, a compaction's firstKeptEntryId remapped); the first message gives a new empty session with parentSession | done | E6 |
| /clone (copy branch) | cmds.c `/clone`; both refuse before the session has a file ("This session has not been saved yet...") as pi does | done | E6 |
| --continue, --session, --resume, --no-session, --name | args.c, app.c, store.c: `-c` the newest file of this directory, `--session` a path or an id prefix, `-r` a numbered list typed at the prompt (pi's picker is a screen; listing only reads the files, as pi's buildSessionInfo), `--no-session` keeps the tree in RAM (the tree commands still work), `-n` | done | E7 |
| Compaction (reserve 16384, keep 20000, cut rules, split-turn) | amiga-pi/compact.c, prompts copied from pi by tools/pi_oracle/mkprompts.py; automatic when the context passes the window minus the reserve, at pi's points (before a prompt's user message and after the run on the last answer's usage, between turns on the projected estimate; agent_compaction.expected is pi's AgentSession over the same runs), and `/compact [instructions]`; settings.json `compaction` {enabled, reserveTokens, keepRecentTokens}. The window is known for Anthropic models (C:Claude's table); other providers have none yet, so they compact only on `/compact` (phase F) | done | E4 |
| Compaction after a context overflow, retry | not built (pi's overflow recovery) | planned | F |
| Branch summary (BRANCH_SUMMARY_PROMPT) | same, prompt text copied | done | E5 |
| Session file location and slug | `<data dir>/sessions/<pi's "--<cwd>--" cut to 30>/<YYYYMMDDTHHMMSS>_<8 hex>.jsonl`. Deliberate difference: an FFS name is at most 30 characters, so pi's `<ISO time>_<uuid>.jsonl` cannot be used; the directory name is pi's cut by C:Claude's rule (the end and a hash). The header keeps pi's uuidv7 `id`; the file name carries its last 8 hex digits. Kept tool output and the gate's permissions file are cut the same way | done | E8 |
| Timestamps | ISO 8601 with milliseconds. Deliberate difference: the Amiga's clock is local time with no zone, so entries end without `Z` or an offset (owner, 2026-10-07 round 2); on the host (UTC) they end in `Z` as pi's do | done | E9 |
| Entry ids | 8 hex digits, collision-checked, from a generator seeded by the clock (pi: random UUID prefix) | done | E1 |
| Opening a session written on the Amiga in pi on a Mac | `tools/pi_oracle/crosscheck.sh` opens the files session3 writes with pi's own SessionManager (host); the Amiga-written file is the owner's check | host done; owner check open | E11 |

## tools

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| read {path, offset?, limit?} | amiga-pi/tools_pi.c run_read as pi's read.ts: the text as it is (no line numbers), split at LF, offset/limit, 2000 lines / 50 KB with pi's offset notices and errors (ENOENT, "Offset N is beyond end of file", the over-long first line's sed hint), no temp file; images by their first bytes (pi's detectSupportedImageMimeType: PNG not animated, JPEG, GIF, WebP, BMP) through amiga-pi/image.c as pi's processImage: BMP to PNG, over 2000x2000 or 4.5 MB of base64 scaled (Lanczos3, EXIF orientation first) and sent as PNG with pi's notes, 3/4 steps until it fits (decoders and PNG writer: stb_image/stb_image_write, vendor/stb; +60 KB on the Amiga). Differs: a file that is not UTF-8 is read as Latin-1 (pi: UTF-8 with U+FFFD); a file over 16 MB is refused (memory); pi also tries JPEG at falling qualities (amiga-pi writes PNG only, so it may scale further) and decodes WebP (amiga-pi passes a WebP through when it fits, else omits it); the non-vision-model note waits for model metadata (F) | done (D, parity) | D1 |
| write {path, content} | claude/tools.c Write under pi_rules: no read first, the directories made; C:Claude's result words | done (D) | D1 |
| edit {path, edits[{oldText,newText}]} | claude/edit_orig.c tools_edit_orig on Edit's loading and write, as pi's edit-diff.ts: BOM kept apart, CRLF matched as LF and restored, exact match first, then pi's fuzzy normalisation (NFKC by claude/unorm.c, Unicode 16.0, white space at line ends, smart quotes, dashes, special spaces) with the untouched lines kept, occurrences counted in the normalised text; against the original, unique, not overlapping, all or none, pi's error words, pi's legacy argument forms; pi's own fuzzy and CRLF/BOM test cases. Deliberate difference: a file that is not UTF-8 is read and written back as Latin-1 (C:Claude's rule; pi would decode it as UTF-8 and write U+FFFD back). Files over 1 MB are refused (Edit's limit) | done (D, parity) | D2 |
| bash {command, timeout?} -> runs vsh -c, AmigaDOS Shell fallback | claude/tools.c Bash under pi_rules through sys->run (sys_amiga: `vsh <script>` when vsh is installed, else Execute in the AmigaShell); starts in the working directory each time, no default time limit, a time limit stops it; pi's status words after the output (the output cut at its tail first, as pi); the prompt says which shell. `vsh <script>` measured against `vsh -c` on the rig (amigapi_rig step 4d/4e): exit status, standard input, the directory, quoting and several lines alike; a 600-character command runs as a script but as a -c argument the Shell refuses the line. Differs: $0 is the script's name (pi's bash -c: bash) | done (D, parity) | D3 |
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
| Theme JSON (appearance, vars, colors) | claude/theme.c pitheme_load: name, vars with chains, the 51 required and 5 optional roles with pi's fallbacks, `""` for the terminal's colour, a 0-255 index, #rgb, #rrggbb, okhsl(), oklch() (amiga-pi/okcolor.c, floating point, checked against pi's parseColor for dark.json and light.json and nine other colours); pi's error texts (missing tokens listed sorted, circular reference, variable not found, bad theme name). `appearance` and `export` are read and not used | done (N6) | N6 |
| Built-in system / dark / light | `system`: pi's table for a terminal that reports nothing (ANSI indices, neutral roles faint, no panel backgrounds), compared with pi's own generator; dark and light: pi's files embedded (themes_builtin.h from tests/pi/tui/*.json). The terminal-palette query tiers of `system` do not exist here | done (N6) | N6 |
| Project themes (.pi/themes, trust-gated) | Themes are read from `<data dir>/themes/<name>.json` and `<agent dir>/themes/<name>.json`; `.pi/themes` of a trusted project and the auto light/dark pair `dark/light` are not read | partial | N6 |

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

Phase K (2026-10-11). Expected outputs from pi's own code: `tools/pi_oracle/mcp_oracle.sh` -> `tests/pi/mcp/*.expected`.

| pi concept | Amiga implementation | status | phase item |
|---|---|---|---|
| Transport stdio | `cl_sys` spawn2: argv (not a shell line), env, cwd; on the Amiga two PTY: pipe pairs (K1, ARCHITECTURE.md). The server's stderr: on the Amiga it goes with its output (System() has no error stream on 3.x), on the host to /dev/null; pi keeps a tail for /mcp | done; differences: stderr | K4, K5 |
| Transport streamable HTTP | `llm_post`: POST, application/json or event-stream answer, Mcp-Session-Id, MCP-Protocol-Version; 404 on a session: initialize again and the request once more. No GET stream (single-threaded: no server messages outside a request), no DELETE at close | done; differences: GET stream, DELETE | K3 |
| Server requests (ping, roots/list) | answered (roots: the session directory as a file URL); over stdio at once, over HTTP after the answer in progress (one connection at a time) | done; difference: HTTP timing | K2 |
| Connect retries | HTTP: after 250 ms and 1 s on a network error, 408, 429, 5xx but 501 (pi's); tool calls not retried | done | K3 |
| Connecting | every enabled server when the session starts, one after the other, before the first prompt (pi: in the background; the first prompt waits up to 10 s for direct servers) | done; difference | K |
| LAN rule for plain http | amiga-pi's rule (ARCHITECTURE.md): plain http only to a LAN address; an Authorization header is not sent over it (one notice) | done; amiga-pi's, not pi's | K3 |
| Legacy SSE transport | refused with pi's words | done | K2 |
| Exposure direct | declared to the model; also amiga-pi's default when a server sets none (pi's default is codemode) | done; difference: default | K6 |
| Exposure deferred | `tool_search` (pi's description, schema, BM25 ranking -- in integer arithmetic, no FPU -- and result words); loaded tools declared from the next call and recorded as tool changes in the session | done | K6 |
| Exposure hidden | never reachable; its names still taken | done | K6 |
| Exposure codemode (default in pi) | not planned: needs a JavaScript engine; a codemode server or tool is reached through tool_search (as pi allows) | not planned | - |
| toolExposure | exact names over patterns, the first matching pattern (`*`), `codemode-deferred` alias | done | K6 |
| mcp_servers section | pi's renderServersSection (4096 characters, 250 per description, the omission line); the reach is always tool_search; made when the session starts (pi re-renders before each prompt and appends a changed one) | done; difference: re-render | K6 |
| mcp.json config | `ENVARC:amiga-pi/mcp.json` with pi's validation and words; `<root>/.pi/mcp.json` with pi's override rule is read only for a trusted project (phase G trust: `-a`, a saved decision, settings.json) | done | K2, G3 |
| env and headers values | `${NAME}` from the environment (unset: empty), a whole value `!command` its output | done | K2 |
| Tool count | pi has no cap on tools; amiga-pi's session holds PI_TOOLS (64): a direct MCP tool that does not fit is left out (the last slot is kept for tool_search), tool_search loads only what fits and says so ("Loaded N tools" counts those) | done; difference: the cap | K, KA2 |
| Tool names mcp__server__tool | pi's createMcpToolName (per UTF-16 unit, 64 characters, sha256 suffix) and registerTools' collision rule | done | K2 |
| Results as model content | text, images, audio placeholder, resource links, embedded text, image and binary resources (binary saved to T:), structuredContent alone as JSON.stringify(..., 2) with numbers as the server wrote them; pi's "Read it with read_mcp_resource" part left out (no resource tools) | done | K2 |
| Result truncation over 20 KB to a temp file | Codex's format; the file `T:amiga-pi-<call id>.txt` (30-character names) where pi writes `pi-mcp-*.txt` | done; difference: file name | K2 |
| Invalid arguments | checked against the server's schema before the call (claude/schema.c), pi's "Validation failed" words | done | K2 |
| Resource tools (list_mcp_resources, list_mcp_resource_templates, read_mcp_resource) | - | not planned in K (no plan item) | - |
| Progress notifications, request timeout | per server (default 60 s), counted from the last byte received; progress notifications are read, not shown | partial | K |
| Tool list changes (notifications/tools/list_changed) | ignored: the list is read when the server connects | not planned in K | - |
| /mcp, pi mcp add/list/remove/login | - | not planned in K | - |
| OAuth with dynamic client registration | - | not planned until probe O1 shows feasibility | O |
| --no-mcp | no servers this run | done | K |
| --tools with mcp__ entries | MCP tools are always kept | not built in K | - |
| clientInfo | `amiga-pi` 0.1 (pi sends `pi` and its version) | done; difference | K2 |

Phase F (2026-10-11). Models and the switch: `/model` is a numbered list (providers with a credential, or a plain-http endpoint) and `/model <number|provider/pattern>` instead of pi's full-screen selector; `/thinking [level]`; `--provider`, `--thinking`, `--api-key`, `--list-models [search]`; `provider/id:level` patterns as pi's resolveCliModel (ids sorted with strcmp, not ICU localeCompare). The switch appends model_change and, only when the clamped level changed, thinking_level_change (pi's setModel); a new session records the real starting level (pi default "medium", "off" for a model without reasoning); resume restores it from the branch (E7's thinking half). Context window and cost come from the registry for every model; the status line shows model, level and context left; `/session` shows cost. A provider the registry does not know with `--base-url` is kept as the old form (not in pi). Not built: Ctrl+P cycling, scoped models, per-model thinking defaults in settings, /login.
