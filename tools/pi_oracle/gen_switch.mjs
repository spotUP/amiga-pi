// Goldens for tests/test_pi_model_reach.c test_switch and test_switch_back: the request pi's own providers send
// after a model switch, from pi-ai's streamSimple (anthropic-messages and openai-responses) with a fake fetch that
// answers with a recorded stream. Inputs (tests/pi/model/switch_inputs.json): the system prompt, the bash description and
// the session id the app sends (the tools are pi's real declarations, $VTCON_DIR/tests/pi/pi_tools.json), and the session id its store makes. Writes tests/pi/model/switch_second.json
// (Anthropic answer, then gpt-4.1) and switch_back_second.json (gpt-4.1 answer, then Anthropic at the level the
// switch clamps off to).
//   VTCON_DIR=<vtcon> [PI_NPM=<pi install>] node tools/pi_oracle/gen_switch.mjs
// Also writes the Anthropic-only conversation tests/pi/model/levels_{first,second,third}.json: opus-5-5 prompted at
// low, high and medium (the system messages pi inserts carry the levels of the earlier answers). The models are
// pi-ai's own data, unaltered: claude-opus-5-5 carries supportsMidConvoEffort / SystemMessages / ToolChanges.
import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const npm = process.env.PI_NPM || "/opt/homebrew/lib/node_modules/@earendil-works/pi-coding-agent";
const vt = process.env.VTCON_DIR || "../vtcon";
const dist = join(npm, "node_modules/@earendil-works/pi-ai/dist");
const imp = (p) => import(pathToFileURL(join(dist, p)).href);
const { MODELS } = await imp("models.generated.js");
const { clampThinkingLevel } = await imp("models.js");
const anth = await imp("api/anthropic-messages.js");
const resp = await imp("api/openai-responses.js");
const { normalizeContext } = await imp("utils/transcript.js");
const here = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "tests", "pi", "model");
const inputs = JSON.parse(readFileSync(join(here, "switch_inputs.json"), "utf8"));
// pi's real tool declarations (constrainedSampling included), made by vtcon's tools/gen_pi_tools.mjs: the requests
// are made from them, never from plain tools
// except the bash description, which amiga-pi words for its shell (COMPATIBILITY.md): inputs.bashDescription
inputs.tools = JSON.parse(readFileSync(join(vt, "tests", "pi", "pi_tools.json"), "utf8"));
for (const t of inputs.tools) if (t.name === "bash") t.description = inputs.bashDescription;
const textSse = readFileSync(join(vt, "tests", "claude", "text.sse"), "utf8");
const anthSse = readFileSync(join(vt, "tests", "pi", "switch_anthropic.sse"), "utf8");
const plainSse = readFileSync(join(vt, "tests", "pi", "responses", "plain.sse"), "utf8");
const opus = MODELS.anthropic["claude-opus-5-5"];
const gpt41 = MODELS.openai["gpt-4.1"];
const user = (t) => ({ role: "user", content: [{ type: "text", text: t }], timestamp: 1760100000000 });

async function run(api, model, messages, level, sse) {
	let body;
	const fetch = async (url, init) => {
		body = init.body;
		return new Response(sse, { status: 200, headers: { "content-type": "text/event-stream" } });
	};
	const context = normalizeContext({ systemPrompt: inputs.systemPrompt, messages, tools: inputs.tools });
	const s = api.streamSimple(model, context, { apiKey: "sk-test-not-real", fetch, reasoning: level === "off" ? undefined : level, sessionId: inputs.sessionId });
	let final;
	for await (const ev of s) if (ev.type === "done" || ev.type === "error") final = ev.type === "done" ? ev.message : ev.error;
	return { body, final };
}

// opus (medium: pi's default), then gpt-4.1 (no reasoning: off)
const a = await run(anth, opus, [user("hello")], "medium", anthSse);
const second = await run(resp, gpt41, [user("hello"), a.final, user("again")], "off", plainSse);
writeFileSync(join(here, "switch_second.json"), second.body);
// gpt-4.1 (off), then opus: the level off is clamped to the model's (off -> low)
const b = await run(resp, gpt41, [user("hello")], "off", plainSse);
const lv = clampThinkingLevel(opus, "off");
const back = await run(anth, opus, [user("hello"), b.final, user("again")], lv, textSse);
writeFileSync(join(here, "switch_back_second.json"), back.body);
// opus-5-5 alone: three prompts at low, high, medium; each request after the first carries the earlier answers'
// levels as system messages
const l1 = await run(anth, opus, [user("hello")], "low", anthSse);
writeFileSync(join(here, "levels_first.json"), l1.body);
const l2 = await run(anth, opus, [user("hello"), l1.final, user("again")], "high", anthSse);
writeFileSync(join(here, "levels_second.json"), l2.body);
const l3 = await run(anth, opus, [user("hello"), l1.final, user("again"), l2.final, user("third")], "medium", anthSse);
writeFileSync(join(here, "levels_third.json"), l3.body);
// gpt-4 (context window 8192, max output 8192): the first request's output cap is clamped by the estimate of the
// prompt and the tools (pi-ai buildBaseOptions / clampMaxTokensToContext)
const g4 = await run(resp, MODELS.openai["gpt-4"], [user("hello")], "off", plainSse);
writeFileSync(join(here, "clamp_first.json"), g4.body);
// opus-5-5 with a tool that adds another during the run (the tests' "adder"): the second request declares the new tool
// as pi's agent loop does, a system message with toolsAdded after the tool result
const adderTool = { name: "adder", description: "Adds the tool late", parameters: { type: "object", properties: {}, required: [] } };
const lateTool = { name: "late", description: "A tool added during the run", parameters: { type: "object", properties: { x: { type: "string" } }, required: ["x"] } };
const adderSse = readFileSync(join(vt, "tests", "pi", "anthropic", "adder.sse"), "utf8");
const inputs0 = inputs.tools;
inputs.tools = [...inputs0, adderTool];
const d1 = await run(anth, opus, [user("hello")], "medium", adderSse);
const call = d1.final.content.find((b) => b.type === "toolCall");
const result = { role: "toolResult", toolCallId: call.id, toolName: "adder", content: [{ type: "text", text: "ok" }], isError: false, timestamp: 1760100000000 };
const declare = { role: "system", content: "", toolsAdded: [lateTool], timestamp: 1760100000000 };
// the second request: the context's tools are the executable ones; the initial system message is the first request's
inputs.tools = [...inputs0, adderTool];
const ctx2 = normalizeContext({ systemPrompt: inputs.systemPrompt, messages: [user("hello"), d1.final, result, declare], tools: inputs.tools });
let body2;
const fetch2 = async (url, init) => { body2 = init.body; return new Response(anthSse, { status: 200, headers: { "content-type": "text/event-stream" } }); };
const st = anth.streamSimple(opus, ctx2, { apiKey: "sk-test-not-real", fetch: fetch2, reasoning: "medium", sessionId: inputs.sessionId });
for await (const ev of st) if (ev.type === "done" || ev.type === "error") break;
writeFileSync(join(here, "declare_second.json"), body2);
console.log("[OK] switch goldens written; off clamps to " + lv + " on opus-5-5");
