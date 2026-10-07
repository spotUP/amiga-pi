---
date: 2026-10-07
topic: pi sources the research did not read (plan item A3)
tags: [amiga-pi, pi, openai-completions, google, bedrock, azure, compaction, themes, auth]
status: final
---

# pi sources addendum (A3)

All permalinks pin pi commit `f10993bc7f28145df1375f3ff39c7f5c4cfc05f0` (earendil-works/pi, HEAD on 2026-10-07).
Files were fetched raw and grepped; line numbers are from that commit. "R3.1" = the research's provider section.

## 1. openai-completions (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/ai/src/api/openai-completions.ts)

Confirmed:
- Request always has `stream: true`; `stream_options: {include_usage: true}` unless compat.supportsUsageInStreaming === false (:835). `store:false` only when compat.supportsStore (:839).
- Token cap: `max_tokens` when compat.maxTokensField == "max_tokens", else `max_completion_tokens`; only sent when options.maxTokens is set (:843-848).
- System role is "developer" only when model.reasoning and compat.supportsDeveloperRole, else "system" (:1233).
- Tool result message carries `tool_call_id` (:1427); assistant tool call carries `name` (:1368, :1377); `requiresToolResultName` adds a name to the tool message.
- Tools: `strict` is written only when compat.supportsStrictMode !== false (:1513); the auto-detected default is supportsStrictMode:false (:1675), so a generic endpoint gets no `strict` key.
- Auto-detected defaults for a generic endpoint (:1640-1690): supportsDeveloperRole true (non-OpenRouter, non-"non-standard"), supportsUsageInStreaming true, maxTokensField max_completion_tokens (max_tokens when the host is detected as one that needs it), requiresToolResultName false, requiresAssistantAfterToolResult false, requiresThinkingAsText false, thinkingFormat "openai". Explicit model.compat overrides the detection (getCompat :1696).
- reasoning_effort = model.thinkingLevelMap[level] ?? level (:971).
Corrections to R3.1: the four flags in plan B6 (maxTokensField, supportsUsageInStreaming, supportsDeveloperRole, requiresToolResultName) are all real and default as above; detection by provider/URL means a bare "ollama" or LAN endpoint takes the "openai" defaults, so B6 must implement the detection order (compat default -> explicit override), not only the flags.

## 2. Tool-call id normalisation (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/ai/src/api/transform-messages.ts, openai-completions.ts:1202)

- transformMessages(messages, model, normalizeToolCallId) runs only for assistant messages NOT from the same model (isSameModel :95); same-model blocks pass untouched (:105-120).
- Cross-model: thinking dropped unless same model with a signature; thoughtSignature deleted from tool calls (:131); ids rewritten through the adapter's normaliser and the matching toolResult.toolCallId is remapped through toolCallIdMap (:136-141).
- openai-completions normaliser: an id with "|" (Responses form {call_id}|{item_id}) becomes {call_id}_{item_id} with [^a-zA-Z0-9_-] replaced by "_", truncated to 40 chars (OpenAI limit) (:1202-1215).
- Orphaned tool calls get a synthetic toolResult with text "No result provided" (:175); a new user turn also triggers synthetic results (:223).

## 3. Google (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/ai/src/api/google-generative-ai.ts, google-shared.ts)

- pi uses the Google SDK; the system prompt goes in `systemInstruction` (:391-394); Gemini has no mid-conversation system messages (google-shared.ts:192).
- thoughtSignature can appear on any part type, not only function calls; later deltas may omit it, so the first-seen value is kept (google-shared.ts:118-135); it is replayed only for the same provider+model (resolveThoughtSignature :235, :249).
- [unverified: SDK-defined] the REST path (`models/<id>:streamGenerateContent?alt=sse`) and the key header (`x-goog-api-key`): no literal appears in pi's source (grep for streamGenerateContent, alt=sse, x-goog-api-key, generativelanguage found nothing in these two files). Must be read from Google's REST reference when phase M starts.

## 4. Bedrock (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/ai/src/api/bedrock-converse-stream.ts)

- pi uses the AWS SDK `ConverseStreamCommand` (:15, :286), so no wire framing is in pi.
- A bearer token (env AWS_BEARER_TOKEN_BEDROCK or option) bypasses SigV4 and sends `Authorization: Bearer <token>` (:106-108, :187). SigV4 headers: `host` and `x-amz-*` take part in the canonical request, `authorization` does not (:462).
- [unverified: SDK-defined] the event-stream binary framing of ConverseStream and the URL `/model/<id>/converse-stream`: not in pi's source; phase M must take them from AWS's API reference.

## 5. Azure OpenAI Responses (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/ai/src/api/azure-openai-config.ts, azure-openai-responses.ts)

- Base URL normalised to `https://<resource>.openai.azure.com/openai/v1` (config :52-69); the SDK appends `/deployments/<model>/...` and `?api-version=v1`.
- Deployment name resolved per model id from a "modelId=deployment" list (config :20-22; resolveDeploymentName responses :45); request `model` is the deployment name (:199).
- [unverified: SDK-defined] the auth header (`api-key`) is set inside the AzureOpenAI SDK; no literal in these files.

## 6. Compaction and branch summary (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/coding-agent/src/core/compaction/)

- Defaults reserveTokens 16384, keepRecentTokens 20000 (compaction.ts:128-129); trigger `contextTokens > contextWindow - reserveTokens` (:269). Confirms R2.2.
- SUMMARIZATION_SYSTEM_PROMPT in utils.ts:161; SUMMARIZATION_PROMPT compaction.ts:507 ("Use this EXACT format": ## Goal / ## Constraints & Preferences / ## Progress (### Done ...) / ...); UPDATE_SUMMARIZATION_PROMPT :577 (incorporates new messages into a `<previous-summary>`); BRANCH_SUMMARY_PROMPT branch-summarization.ts:258 (same headings). The prompt texts are copied into amiga-pi at phase E from these lines, not from memory.

## 7. Auth (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/coding-agent/src/core/auth-storage.ts)

auth.json is an object keyed by provider id (:225-250). Each value is `{"type":"api_key","key"?:string,"env"?:{string:string}}` or `{"type":"oauth","access":string,"refresh":string,"expires":number}`. A key may be a command value (isCommandConfigValue :263). Confirms R2.1 order (--api-key > auth.json > models.json > env).

## 8. Themes and keybindings (https://github.com/earendil-works/pi/blob/f10993bc7f28145df1375f3ff39c7f5c4cfc05f0/packages/coding-agent/docs/themes.md, keybindings.md)

- Theme: JSON file `<agent-dir>/themes/<name>.json` (project: `.pi/themes/`, trust-gated); fields `appearance` ("dark"|"light", optional), `vars` (named values, may chain), `colors` (required, interface roles); a value is a hex string, a 0-255 index, a var name, or terminal default; built-ins system (default), dark, light; setting `theme` in settings.json.
- Keybindings: `<agent-dir>/keybindings.json`, an object {action id: key | [keys]}, an empty list disables; ids like `tui.editor.cursorUp`, `app.session.tree`; reload with /reload. Defaults table in keybindings.md.
- The exact colour-role list is in the repo's `theme-schema.json` (not fetched; [unverified]); phase N reads it.

## 9. Not read

`openai-responses*.ts`, `mistral`, `pi-messages`, OAuth flows (phase O), `settings-manager.ts` field list, `system-prompt.ts` default prompt text (phase G reads it).
