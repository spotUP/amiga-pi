// Goldens for tests/test_pi_model_reach.c test_declare_every_model: what pi's agent loop (pi-agent-core runAgentLoop,
// message_end events are what AgentSession writes to the session file) puts in the transcript when a tool adds
// another during a run, on a model without mid-conversation system messages (openai-responses gpt-4.1, requests
// made by pi-ai's own provider with a fake fetch). The tool "adder" pushes "late" onto the context's tools; pi's
// declareToolChanges then appends a system message with toolsAdded after the tool result, before the next request.
// Writes tests/pi/model/declare_roles.txt (the roles in message_end order), declare_message.json (the declaring system
// message, timestamp 0), declare_first_tools.json and declare_second_tools.json (the "tools" the two requests carry), and the
// declare55_* files of a model that does take system messages in the conversation (below).
//   VTCON_DIR=<vtcon> node tools/pi_oracle/gen_declare.mjs
import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { npmRoot } from "./pin.mjs";
const vt = process.env.VTCON_DIR || "../vtcon";
const ai = join(npmRoot, "@earendil-works/pi-ai/dist");
const core = join(npmRoot, "@earendil-works/pi-agent-core/dist");
const imp = (p) => import(pathToFileURL(p).href);
const { MODELS } = await imp(join(ai, "models.generated.js"));
const resp = await imp(join(ai, "api/openai-responses.js"));
const { normalizeContext, toToolDeclaration } = await imp(join(ai, "utils/transcript.js"));
const { runAgentLoop } = await imp(join(core, "agent-loop.js"));
const here = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "tests", "pi", "model");
const inputs = JSON.parse(readFileSync(join(here, "switch_inputs.json"), "utf8"));
const adderSse = readFileSync(join(vt, "tests", "pi", "responses", "adder.sse"), "utf8");
const plainSse = readFileSync(join(vt, "tests", "pi", "responses", "plain.sse"), "utf8");
const model = MODELS.openai["gpt-4.1"];
async function runLoop(model) {
	const late = { name: "late", label: "late", description: "A tool added during the run", parameters: { type: "object", properties: { x: { type: "string" } }, required: ["x"] }, execute: async () => ({ content: [], details: {} }) };
	const real = JSON.parse(readFileSync(join(vt, "tests", "pi", "pi_tools.json"), "utf8"));
	for (const t of real) if (t.name === "bash") t.description = inputs.bashDescription;     // worded for amiga-pi's shell
	const tools = real.map((t) => ({ ...t, label: t.name, execute: async () => ({ content: [], details: {} }) }));
	const adder = { name: "adder", label: "adder", description: "Adds the tool late", parameters: { type: "object", properties: {}, required: [] },
		execute: async () => { tools.push(late); return { content: [{ type: "text", text: "ok" }], details: {} }; } };
	tools.push(adder);
	const bodies = [];
	const sses = [adderSse, plainSse];
	const fetch = async (url, init) => {
		bodies.push(init.body);
		return new Response(sses[bodies.length - 1], { status: 200, headers: { "content-type": "text/event-stream" } });
	};
	const streamFn = (m, context, options) => resp.streamSimple(m, context, { ...options, apiKey: "sk-test-not-real", fetch, sessionId: inputs.sessionId });
	const initial = normalizeContext({ systemPrompt: inputs.systemPrompt, messages: [], tools: tools.map(toToolDeclaration) }).messages;
	const context = { messages: [...initial], tools };
	const events = [];
	await runAgentLoop([{ role: "user", content: [{ type: "text", text: "hello" }], timestamp: 1760100000000 }], context,
		{ model, convertToLlm: (ms) => ms, reasoning: undefined },
		(e) => { if (e.type === "message_end") events.push(e.message); }, undefined, streamFn);
	return { events, bodies };
}
const { events, bodies } = await runLoop(model);
const sys = events.filter((m) => m.role === "system").map((m) => ({ ...m, timestamp: 0 }));
writeFileSync(join(here, "declare_roles.txt"), events.map((m) => m.role).join(" ") + "\n");
writeFileSync(join(here, "declare_message.json"), JSON.stringify(sys[0]) + "\n");
writeFileSync(join(here, "declare_first_tools.json"), JSON.stringify(JSON.parse(bodies[0]).tools) + "\n");
writeFileSync(join(here, "declare_second_tools.json"), JSON.stringify(JSON.parse(bodies[1]).tools) + "\n");
// gpt-5.5 (supportsMidConvoSystemMessages and supportsAdditionalTools): the later system message stays in the transcript, the
// late tool arrives as an additional_tools item of the second request's input, the tools member stays the initial one.
// declare55_roles.txt, declare55_first.json / declare55_second.json: the "input" and "tools" members of the two requests.
{
	const r = await runLoop(MODELS.openai["gpt-5.5"]);
	const pick = (b) => JSON.stringify({ input: JSON.parse(b).input, tools: JSON.parse(b).tools }) + "\n";
	writeFileSync(join(here, "declare55_roles.txt"), r.events.map((m) => m.role).join(" ") + "\n");
	writeFileSync(join(here, "declare55_first.json"), pick(r.bodies[0]));
	writeFileSync(join(here, "declare55_second.json"), pick(r.bodies[1]));
}
console.log("[OK] declare goldens: " + events.map((m) => m.role).join(" "));
