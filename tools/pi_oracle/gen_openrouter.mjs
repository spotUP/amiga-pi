// P12-A10: pi 1.2.0's personalizeOpenRouterCatalog (ai/src/providers/openrouter.ts) against a local fake GET /models/user, one case per
// answer; the models it keeps, with the limits and prices it gives them, are the golden of tests/test_pi_radius.c (the C side feeds the
// same bodies through a stub transport). No network beyond 127.0.0.1: no key, no openrouter.ai.
//   node tools/pi_oracle/gen_openrouter.mjs      (PI_DIST: the pinned install, default vendor/pi-1.2.0 of this checkout or the main one)
import { writeFileSync, existsSync } from "node:fs";
import { createServer } from "node:http";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const cands = [process.env.PI_DIST, join(root, "vendor/pi-1.2.0"), join(root, "../amiga-pi/vendor/pi-1.2.0")].filter(Boolean);
const pin = cands.find((c) => existsSync(join(c, "node_modules/@earendil-works/pi-ai/dist/providers/openrouter.js")));
if (!pin) throw new Error("gen_openrouter: no pinned pi 1.2.0");
const ai = join(pin, "node_modules/@earendil-works/pi-ai/dist/providers");
const { personalizeOpenRouterCatalog } = await import(pathToFileURL(join(ai, "openrouter.js")).href);
const { OPENROUTER_MODELS } = await import(pathToFileURL(join(ai, "openrouter.models.js")).href);
const ids = ["moonshotai/kimi-k2.6", "openai/gpt-5.5", "google/gemini-3.1-pro-preview", "deepseek/deepseek-v3.2", "x-ai/grok-4.7"];
const catalog = ids.map((id) => OPENROUTER_MODELS[id]);
const noise = { name: "x \" } ] { [ y", description: "text with } and ] and \\ and \" quotes { [ ,:", architecture: { modality: "text->text", input_modalities: ["text"] }, supported_parameters: ["tools", "reasoning"] };
const list = {
	data: [
		{ id: "anthropic/not-in-the-catalog", context_length: 1, ...noise },
		{ id: "moonshotai/kimi-k2.6", context_length: 300000, top_provider: { context_length: 262144, max_completion_tokens: 100000, is_moderated: false }, pricing: { prompt: "0.0000007", completion: "0.0000035", input_cache_read: "0.00000016", input_cache_write: "0", overrides: [{ min_prompt_tokens: 200000, prompt: "0.0000014" }] }, ...noise },
		{ id: "openai/gpt-5.5", context_length: 400000, top_provider: { context_length: null, max_completion_tokens: null }, pricing: { prompt: "-1", completion: "-1", input_cache_read: "0.0000005" }, ...noise },
		{ id: "deepseek/deepseek-v3.2", context_length: 128000, top_provider: { max_completion_tokens: 0 }, ...noise },
		{ id: "x-ai/grok-4.7", pricing: { prompt: "0.000002", completion: "bad" }, ...noise },
	],
};
const cases = [
	{ name: "list", status: 200, body: JSON.stringify(list) },
	{ name: "pretty", status: 200, body: JSON.stringify(list, null, 2) },
	{ name: "empty", status: 200, body: JSON.stringify({ data: [] }) },
	{ name: "status401", status: 401, body: '{"error":{"message":"No auth"}}' },
	{ name: "status403", status: 403, body: "{}" },
	{ name: "status404", status: 404, body: "not found" },
	{ name: "status500", status: 500, body: "boom" },
	{ name: "nodata", status: 200, body: '{"other":[]}' },
	{ name: "notjson", status: 200, body: "<html>" },
];
const results = [];
for (const c of cases) {
	let sent = 0;
	const srv = createServer((req, res) => {
		sent = req.url === "/models/user" && req.headers.authorization === "Bearer k" && req.headers.accept === "application/json" ? 1 : -1;
		res.writeHead(c.status, { "content-type": "application/json" });
		res.end(c.body);
	});
	await new Promise((r) => srv.listen(0, "127.0.0.1", r));
	const base = `http://127.0.0.1:${srv.address().port}`;
	let expected;
	try {
		const out = await personalizeOpenRouterCatalog(catalog, { credential: { type: "api_key", key: "k" }, baseUrl: base, signal: new AbortController().signal });
		expected = { result: out.map((m) => ({ id: m.id, contextWindow: m.contextWindow, maxTokens: m.maxTokens, cost: { input: m.cost.input, output: m.cost.output, cacheRead: m.cost.cacheRead, cacheWrite: m.cost.cacheWrite } })) };
	} catch (e) {
		expected = { error: String(e.message) };
	}
	srv.close();
	if (sent !== 1) throw new Error(`${c.name}: the request was not GET /models/user with the key (${sent})`);
	results.push({ ...c, expected });
}
// the catalog before: the C side starts from the shipped one
const before = catalog.map((m) => ({ id: m.id, contextWindow: m.contextWindow, maxTokens: m.maxTokens, cost: { input: m.cost.input, output: m.cost.output, cacheRead: m.cost.cacheRead, cacheWrite: m.cost.cacheWrite } }));
// a key-less call returns the catalog: pi's `if (!key) return catalog`
const dir = join(root, "tests", "pi", "openrouter");
writeFileSync(join(dir, "cases.json"), JSON.stringify({ pi: "1.2.0", catalog: ids, before, cases: results }, null, 1) + "\n");
console.log("wrote", results.length, "cases");
