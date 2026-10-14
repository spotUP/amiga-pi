// Goldens for tests/test_pi_radius.c: what pi's own pi-messages API (pi-ai api/pi-messages.js, the Radius wire) sends and stores for
// a turn driven by pi's agent loop (pi-agent-core runAgentLoop), against a fake fetch that answers the scripted gateway events.
// Per scenario <name>: <name>.sse (the gateway's answers, one stream per request, separated by a line "----"), <name>.body.<i>.json
// (request bodies, timestamps masked), <name>.head.json (authorization, accept, content-type of request 0), <name>.messages.jsonl
// (the assistant messages of message_end, timestamps and durations masked), <name>.url (path and query of request 0).
//   VTCON_DIR=<vtcon> [PI_NPM=<pi install>] node tools/pi_oracle/gen_pi_messages.mjs
import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const here0 = dirname(fileURLToPath(import.meta.url));
const npm = process.env.PI_NPM || join(here0, "../../vendor/pi-1.1.0/node_modules/@earendil-works/pi-coding-agent");
const vt = process.env.VTCON_DIR || "../vtcon";
import { existsSync } from "node:fs";
const nested = join(npm, "node_modules/@earendil-works/pi-ai/dist");
const ai = existsSync(nested) ? nested : join(npm, "../pi-ai/dist");     // the oracle install hoists pi-ai
const core = existsSync(nested) ? join(npm, "node_modules/@earendil-works/pi-agent-core/dist") : join(npm, "../pi-agent-core/dist");
const imp = (p) => import(pathToFileURL(p).href);
const pm = await imp(join(ai, "api/pi-messages.js"));
const { RADIUS_MODELS } = await imp(join(ai, "providers/radius.models.js"));
const { normalizeContext, toToolDeclaration } = await imp(join(ai, "utils/transcript.js"));
const { runAgentLoop } = await imp(join(core, "agent-loop.js"));
const out = join(here0, "..", "..", "tests", "pi", "radius");
const inputs = JSON.parse(readFileSync(join(here0, "..", "..", "tests", "pi", "model", "switch_inputs.json"), "utf8"));
const model = RADIUS_MODELS.balanced;
const sectionsIn = JSON.parse(readFileSync(join(out, "inputs.json"), "utf8"));
const mask = (s) => s.replace(/"timestamp":\d+/g, '"timestamp":0').replace(/"durationMs":\d+/g, '"durationMs":0');
const usage = { input: 120, output: 7, cacheRead: 3, cacheWrite: 0, totalTokens: 130, cost: { input: 0.00036, output: 0.000105, cacheRead: 9e-7, cacheWrite: 0, total: 0.000465 } };
const sse = (events) => events.map((e) => `data: ${JSON.stringify(e)}\n\n`).join("");
const scenarios = {
	plain: { answers: [[
		{ type: "start" }, { type: "text_start", contentIndex: 0 }, { type: "text_delta", contentIndex: 0, delta: "Hel" },
		{ type: "text_delta", contentIndex: 0, delta: "lo" }, { type: "text_end", contentIndex: 0, content: "Hello" },
		{ type: "done", reason: "stop", usage, responseId: "resp_1", providerThinkingLevel: "high" } ]] },
	tools: { answers: [[
		{ type: "start" }, { type: "thinking_start", contentIndex: 0 }, { type: "thinking_delta", contentIndex: 0, delta: "Let me " },
		{ type: "thinking_delta", contentIndex: 0, delta: "look." }, { type: "thinking_end", contentIndex: 0, content: "Let me look.", contentSignature: "sig-abc" },
		{ type: "toolcall_start", contentIndex: 1, id: "call_1", toolName: "read" }, { type: "toolcall_delta", contentIndex: 1, delta: '{"path":' },
		{ type: "toolcall_delta", contentIndex: 1, delta: '"a.txt"}' },
		{ type: "toolcall_end", contentIndex: 1, toolCall: { type: "toolCall", id: "call_1", name: "read", arguments: { path: "a.txt" } } },
		{ type: "done", reason: "toolUse", usage, responseId: "resp_2", providerThinkingLevel: "high",
			rewrite: { policyId: "pol-1", policyVersion: 3, changed: true, tokenCountChange: -12, messageCountChange: -1, systemPromptChanged: false } } ],
		[ { type: "start" }, { type: "text_start", contentIndex: 0 }, { type: "text_delta", contentIndex: 0, delta: "Done." },
		{ type: "text_end", contentIndex: 0, content: "Done.", contentSignature: "{\"v\":1,\"id\":\"m1\"}" },
		{ type: "done", reason: "stop", usage, responseId: "resp_3" } ]] },
	replace: { answers: [[
		{ type: "start" }, { type: "text_start", contentIndex: 0 }, { type: "text_delta", contentIndex: 0, delta: "draft" },
		{ type: "text_end", contentIndex: 0, content: "final" },
		{ type: "thinking_start", contentIndex: 1 }, { type: "thinking_end", contentIndex: 1, content: "", contentSignature: "enc", redacted: true },
		{ type: "toolcall_start", contentIndex: 2, id: "c9", toolName: "read" }, { type: "toolcall_delta", contentIndex: 2, delta: '{"path": "x"' },
		{ type: "toolcall_end", contentIndex: 2, toolCall: { type: "toolCall", id: "c9", name: "read", arguments: { path: "y" }, thoughtSignature: "ts" } },
		{ type: "done", reason: "toolUse", usage, responseId: "resp_4" } ],
		[ { type: "start" }, { type: "text_start", contentIndex: 0 }, { type: "text_end", contentIndex: 0, content: "ok" }, { type: "done", reason: "stop", usage } ]] },
	error_event: { answers: [[
		{ type: "start" }, { type: "text_start", contentIndex: 0 }, { type: "text_delta", contentIndex: 0, delta: "part" },
		{ type: "error", reason: "error", usage, errorMessage: "Upstream failed", responseId: "resp_5" } ]] },
	cut: { answers: [[ { type: "start" }, { type: "text_start", contentIndex: 0 }, { type: "text_delta", contentIndex: 0, delta: "partial" } ]] },
	http401: { status: 401, statusText: "Unauthorized", rawBody: JSON.stringify({ error: { message: "Token expired", code: "unauthorized" } }), answers: [[]] },
	http500: { status: 500, statusText: "Internal Server Error", rawBody: "upstream exploded", answers: [[]] },
};
const real = JSON.parse(readFileSync(join(vt, "tests", "pi", "pi_tools.json"), "utf8"));
for (const t of real) if (t.name === "bash") t.description = inputs.bashDescription;
for (const [name, sc] of Object.entries(scenarios)) {
	const tools = real.map((t) => ({ ...t, label: t.name, execute: async () => ({ content: [{ type: "text", text: "ok" }], details: {} }) }));
	const bodies = [], heads = [], urls = [], messages = [];
	const fetch = async (url, init) => {
		const i = bodies.length;
		bodies.push(init.body); heads.push(init.headers); urls.push(`${url.pathname}${url.search}`);
		if (sc.status) return new Response(sc.rawBody, { status: sc.status, statusText: sc.statusText, headers: { "content-type": "application/json" } });
		return new Response(sse(sc.answers[i]), { status: 200, headers: { "content-type": "text/event-stream" } });
	};
	const streamFn = (m, context, options) => pm.streamSimple(m, context, { ...options, apiKey: "radius-test-token", fetch, sessionId: sectionsIn.sessionId });
	// the coding agent's transcript opens with a structured system message (core/system-prompt.ts buildSystemPromptState: content "", the
	// prompt in sections) that also declares the tools; the sections are amiga-pi's own text (tests/pi/radius/inputs.json)
	const initial = [{ role: "system", content: "", sections: sectionsIn.sections, timestamp: 0, toolsAdded: tools.map(toToolDeclaration) }];
	const context = { messages: [...initial], tools };
	await runAgentLoop([{ role: "user", content: [{ type: "text", text: "hello" }], timestamp: 1760100000000 }], context,
		{ model, convertToLlm: (ms) => ms, reasoning: "high" },
		(e) => { if (e.type === "message_end" && e.message.role === "assistant") messages.push(e.message); }, undefined, streamFn);
	writeFileSync(join(out, `${name}.sse`), sc.answers.map(sse).join("----\n"));
	if (sc.status) writeFileSync(join(out, `${name}.http`), `${sc.status} ${sc.statusText}\n${sc.rawBody}\n`);
	bodies.forEach((b, i) => writeFileSync(join(out, `${name}.body.${i}.json`), mask(b) + "\n"));
	writeFileSync(join(out, `${name}.head.json`), JSON.stringify({ authorization: heads[0].authorization, accept: heads[0].accept, "content-type": heads[0]["content-type"] }) + "\n");
	writeFileSync(join(out, `${name}.url`), urls[0] + "\n");
	writeFileSync(join(out, `${name}.messages.jsonl`), messages.map((m) => mask(JSON.stringify(m))).join("\n") + "\n");
	console.log(name, "requests", bodies.length, "messages", messages.length);
}
