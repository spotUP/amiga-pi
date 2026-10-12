// gen_model.ts -- the expected outputs of tests/test_pi_model.c (plan phase F), produced by pi's own code
// (pi f10993bc): transformMessages (ai/api/transform-messages.ts), shortHash (utils/hash.ts),
// getSupportedThinkingLevels / clampThinkingLevel (models.ts), parseModelPattern (model-resolver.ts) and
// resolveConfigValue (resolve-config-value.ts). Functions that sit in files with heavy imports are cut out of
// the real source text, not rewritten.
//
//   PI_SRC=<dir with the fetched pi files> node tools/pi_oracle/gen_model.ts
//
// PI_SRC defaults to ~/Code/amiga-pi/build/pi-oracle/f (build/pi-oracle/fetch2.py fetches them).
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const out = resolve(here, "..", "..", "tests", "pi", "model");
const src = process.env.PI_SRC ?? join(process.env.HOME!, "Code/amiga-pi/build/pi-oracle/f");
const tmp = mkdtempSync(join(tmpdir(), "pimodel."));
mkdirSync(out, { recursive: true });

const imp = (p: string) => import(pathToFileURL(p).href);
const { transformMessages } = await imp(join(src, "ai__src__api__transform-messages.ts"));
const { shortHash } = await imp(join(src, "ai__src__utils__hash.ts"));

// ---- pieces cut out of the real files ----
function cut(file: string, from: string, to: string): string {
	const t = readFileSync(join(src, file), "utf8");
	const a = t.indexOf(from);
	const b = t.indexOf(to, a);
	if (a < 0 || b < 0) throw new Error(`cannot cut ${from} from ${file}`);
	return t.slice(a, b);
}
const thinkSrc = cut("ai__src__models.ts", "const EXTENDED_THINKING_LEVELS", "/**\n * Check if two models are equal");
writeFileSync(join(tmp, "think.ts"), thinkSrc);
const { getSupportedThinkingLevels, clampThinkingLevel } = await imp(join(tmp, "think.ts"));

const resSrc = cut("coding-agent__src__core__model-resolver.ts", "export const defaultModelPerProvider",
	"export interface ModelScopeDiagnostic");
writeFileSync(join(tmp, "resolver.ts"), `const VALID = ["off","minimal","low","medium","high","xhigh","max"];
function isValidThinkingLevel(l: string) { return VALID.includes(l); }
${resSrc}
`);
const { parseModelPattern } = await imp(join(tmp, "resolver.ts"));

mkdirSync(join(tmp, "core"));
mkdirSync(join(tmp, "utils"));
copyFileSync(join(src, "coding-agent__src__core__resolve-config-value.ts"), join(tmp, "core", "resolve-config-value.ts"));
writeFileSync(join(tmp, "utils", "shell.ts"), `export function getShellConfig() { return { shell: "/bin/sh", args: ["-c"], commandTransport: "args" }; }`);
const { resolveConfigValueUncached } = await imp(join(tmp, "core", "resolve-config-value.ts"));

// ---- canonical JSON: keys sorted ----
function canon(v: any): string {
	if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
	if (v && typeof v === "object") {
		return "{" + Object.keys(v).sort().filter((k) => v[k] !== undefined).map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
	}
	return JSON.stringify(v);
}

// ---- handoff ----
const NOW = 1760100000000;
const realNow = Date.now;
Date.now = () => NOW;
const usage = { input: 1, output: 1, cacheRead: 0, cacheWrite: 0, totalTokens: 2, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
const asst = (api: string, provider: string, model: string, content: any, stopReason = "stop") =>
	({ role: "assistant", content, api, provider, model, usage, stopReason, timestamp: 1760000000000 });
const user = (content: any) => ({ role: "user", content, timestamp: 1760000000000 });
const tres = (id: string, name: string, content: any, isError = false) =>
	({ role: "toolResult", toolCallId: id, toolName: name, content, isError, timestamp: 1760000000000 });
const OPUS = ["anthropic-messages", "anthropic", "claude-opus-5-5"];
const SONNET = ["anthropic-messages", "anthropic", "claude-sonnet-5-5"];
const GPT = ["openai-completions", "openai", "gpt-5.5"];
const QWEN = ["openai-completions", "ollama", "qwen2.5-coder:7b"];
const png = { type: "image", data: "iVBORw0KGgo=", mimeType: "image/png" };
const long450 = "call_abc123|" + "X+/=".repeat(110);
const tgt = (m: string[], image = true) => ({ api: m[0], provider: m[1], id: m[2], input: image ? ["text", "image"] : ["text"] });

const cases: any[] = [
	{ name: "anthropic_to_openai", target: tgt(GPT), messages: [
		user("read S:Startup-Sequence"),
		asst(OPUS[0], OPUS[1], OPUS[2], [
			{ type: "thinking", thinking: "I should read it.", thinkingSignature: "sigA==" },
			{ type: "text", text: "Let me look.", textSignature: "tsig" },
			{ type: "toolCall", id: "toolu_01A-b_C", name: "read", arguments: { path: "S:Startup-Sequence" }, thoughtSignature: "thought" },
		], "toolUse"),
		tres("toolu_01A-b_C", "read", [{ type: "text", text: "SetPatch QUIET\n" }]),
		asst(OPUS[0], OPUS[1], OPUS[2], [{ type: "text", text: "It runs SetPatch." }]),
		user("thanks"),
	] },
	{ name: "openai_to_anthropic", target: tgt(OPUS), messages: [
		user("list S:"),
		asst(GPT[0], GPT[1], GPT[2], [
			{ type: "thinking", thinking: "list it", thinkingSignature: "reasoning_content" },
			{ type: "thinking", thinking: "   ", thinkingSignature: "reasoning" },
			{ type: "toolCall", id: "call_9$x.y", name: "ls", arguments: { path: "S:" } },
		], "toolUse"),
		tres("call_9$x.y", "ls", [{ type: "text", text: "Startup-Sequence\n" }]),
		asst(GPT[0], GPT[1], GPT[2], [{ type: "text", text: "One file." }]),
	] },
	{ name: "same_model_unchanged", target: tgt(OPUS), messages: [
		user("hi"),
		asst(OPUS[0], OPUS[1], OPUS[2], [
			{ type: "thinking", thinking: "hmm", thinkingSignature: "sig1" },
			{ type: "thinking", thinking: "", thinkingSignature: "sig2" },
			{ type: "thinking", thinking: "[Reasoning redacted]", thinkingSignature: "opaque", redacted: true },
			{ type: "text", text: "hello", textSignature: "ts" },
			{ type: "toolCall", id: "toolu_1", name: "read", arguments: {}, thoughtSignature: "keep" },
		], "toolUse"),
		tres("toolu_1", "read", [{ type: "text", text: "x" }]),
	] },
	{ name: "other_anthropic_model", target: tgt(SONNET), messages: [
		user("hi"),
		asst(OPUS[0], OPUS[1], OPUS[2], [
			{ type: "thinking", thinking: "signed thought", thinkingSignature: "sig1" },
			{ type: "thinking", thinking: "opaque", thinkingSignature: "red", redacted: true },
			{ type: "thinking", thinking: "", thinkingSignature: "sig3" },
			{ type: "text", text: "hello" },
		]),
	] },
	{ name: "pipe_ids_to_openai", target: tgt(GPT), messages: [
		user("go"),
		asst("openai-responses", "openai", "gpt-5", [
			{ type: "toolCall", id: "call_ab+c|item/9=", name: "read", arguments: {} },
			{ type: "toolCall", id: long450, name: "ls", arguments: {} },
			{ type: "toolCall", id: "call_abc|", name: "ls", arguments: {} },
		], "toolUse"),
		tres("call_ab+c|item/9=", "read", [{ type: "text", text: "a" }]),
		tres(long450, "ls", [{ type: "text", text: "b" }]),
		tres("call_abc|", "ls", [{ type: "text", text: "c" }]),
	] },
	{ name: "long_id_to_openai_provider", target: tgt(GPT), messages: [
		user("go"),
		asst(QWEN[0], QWEN[1], QWEN[2], [{ type: "toolCall", id: "c".repeat(60), name: "read", arguments: {} }], "toolUse"),
		tres("c".repeat(60), "read", [{ type: "text", text: "a" }]),
	] },
	{ name: "long_id_to_other_openai_compatible", target: tgt(QWEN), messages: [
		user("go"),
		asst(OPUS[0], OPUS[1], OPUS[2], [{ type: "toolCall", id: "c".repeat(60), name: "read", arguments: {} }], "toolUse"),
		tres("c".repeat(60), "read", [{ type: "text", text: "a" }]),
	] },
	{ name: "long_id_to_anthropic", target: tgt(OPUS), messages: [
		user("go"),
		asst(GPT[0], GPT[1], GPT[2], [{ type: "toolCall", id: "z".repeat(70) + "!", name: "read", arguments: {} }], "toolUse"),
		tres("z".repeat(70) + "!", "read", [{ type: "text", text: "a" }]),
	] },
	{ name: "images_without_vision", target: tgt(QWEN, false), messages: [
		user([{ type: "text", text: "look" }, png, png, { type: "text", text: "and" }, png]),
		asst(QWEN[0], QWEN[1], QWEN[2], [{ type: "toolCall", id: "t1", name: "read", arguments: {} }], "toolUse"),
		tres("t1", "read", [png, png, { type: "text", text: "(tool image omitted: model does not support images)" }, png]),
	] },
	{ name: "images_with_vision", target: tgt(OPUS), messages: [
		user([{ type: "text", text: "look" }, png]),
	] },
	{ name: "errors_and_orphans", target: tgt(OPUS), messages: [
		user("one"),
		asst(OPUS[0], OPUS[1], OPUS[2], [{ type: "toolCall", id: "o1", name: "read", arguments: {} }], "toolUse"),
		user("two"),
		asst(OPUS[0], OPUS[1], OPUS[2], [{ type: "text", text: "half" }], "error"),
		asst(OPUS[0], OPUS[1], OPUS[2], [{ type: "toolCall", id: "o2", name: "ls", arguments: {} }], "toolUse"),
		{ role: "system", content: "", sections: { a: "b" }, timestamp: 1760000000000 },
		tres("o2", "ls", [{ type: "text", text: "r" }]),
		user("three"),
		asst(OPUS[0], OPUS[1], OPUS[2], [{ type: "toolCall", id: "o3", name: "ls", arguments: {} }], "toolUse"),
	] },
	{ name: "null_content", target: tgt(OPUS), messages: [
		{ role: "user", content: null, timestamp: 1760000000000 },
		{ ...asst(OPUS[0], OPUS[1], OPUS[2], null), content: null },
	] },
];
writeFileSync(join(out, "handoff.cases"), cases.map((c) => JSON.stringify(c)).join("\n") + "\n");
const idFn = (target: any) => (id: string) => {
	// the same normaliser each api's convertMessages hands to transformMessages
	if (target.api === "anthropic-messages") return id.replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 64);
	if (target.api === "openai-completions") {
		if (id.includes("|")) {
			const i = id.indexOf("|");
			const callId = id.slice(0, i).replace(/[^a-zA-Z0-9_-]/g, "_");
			const itemId = id.slice(i + 1).replace(/[^a-zA-Z0-9_-]/g, "_");
			const combined = itemId.length > 0 ? `${callId}_${itemId}` : callId;
			if (combined.length <= 40) return combined;
			const hash = shortHash(id).slice(0, 8);
			const prefix = callId.slice(0, Math.max(1, 40 - hash.length - 1));
			return `${prefix}_${hash}`;
		}
		if (target.provider === "openai") return id.length > 40 ? id.slice(0, 40) : id;
		return id;
	}
	return id;
};
let hexp = "";
for (const c of cases) {
	const res = transformMessages(structuredClone(c.messages), c.target, idFn(c.target));
	hexp += c.name + "\t" + canon(res) + "\n";
}
writeFileSync(join(out, "handoff.expected"), hexp);
Date.now = realNow;

// ---- shortHash ----
const hashIn = ["", "a", "call_abc|" + "X".repeat(100), "toolu_01A-b_C", "0123456789"];
writeFileSync(join(out, "hash.expected"), hashIn.map((s) => `${s}\t${shortHash(s)}`).join("\n") + "\n");

// ---- thinking levels ----
const models = {
	providers: {
		t: {
			baseUrl: "http://localhost/v1", api: "openai-completions",
			models: [
				{ id: "plain" },
				{ id: "reason", reasoning: true },
				{ id: "reason-x", reasoning: true, thinkingLevelMap: { xhigh: "xhigh", max: "max" } },
				{ id: "reason-nomin", reasoning: true, thinkingLevelMap: { minimal: null, low: "lo", xhigh: "xh" } },
				{ id: "reason-nooff", reasoning: true, thinkingLevelMap: { off: null } },
				{ id: "reason-onlymax", reasoning: true, thinkingLevelMap: { minimal: null, low: null, medium: null, high: null, max: "max" } },
			],
		},
	},
};
writeFileSync(join(out, "think.models"), JSON.stringify(models, null, 1) + "\n");
const LEVELS = ["off", "minimal", "low", "medium", "high", "xhigh", "max"];
let texp = "";
for (const m of models.providers.t.models as any[]) {
	const pm = { reasoning: false, ...m };
	texp += `${m.id} supported ${getSupportedThinkingLevels(pm).join(",")}\n`;
	for (const l of LEVELS) texp += `${m.id} clamp ${l} ${clampThinkingLevel(pm, l)}\n`;
}
writeFileSync(join(out, "think.expected"), texp);

// ---- model patterns ----
const rmodels = {
	providers: {
		anthropic: { baseUrl: "https://api.anthropic.com", api: "anthropic-messages", models: [
			{ id: "claude-opus-5-5", name: "Claude Opus 5.5" }, { id: "claude-opus-5", name: "Claude Opus 5" },
			{ id: "claude-sonnet-5-5", name: "Claude Sonnet 5.5" }, { id: "claude-sonnet-5-5-20260101", name: "Claude Sonnet 5.5 (dated)" },
			{ id: "claude-haiku-4-5-20251001", name: "Claude Haiku 4.5" }, { id: "claude-haiku-4-5-latest", name: "Claude Haiku 4.5" },
		] },
		openai: { baseUrl: "https://api.openai.com/v1", api: "openai-completions", models: [
			{ id: "gpt-5.5", name: "GPT-5.5" }, { id: "gpt-4.1", name: "GPT-4.1" }, { id: "gpt-4.1-mini", name: "GPT-4.1 mini" },
		] },
		ollama: { baseUrl: "http://localhost:11434/v1", api: "openai-completions", models: [
			{ id: "qwen2.5-coder:7b", name: "Qwen2.5 Coder 7B" }, { id: "llama3.1:8b", name: "Llama 3.1 8B" },
			{ id: "foo:high", name: "Colon model" },
		] },
	},
};
writeFileSync(join(out, "resolve.models"), JSON.stringify(rmodels, null, 1) + "\n");
const all: any[] = [];
for (const [p, cfg] of Object.entries(rmodels.providers) as any) for (const m of cfg.models) all.push({ provider: p, id: m.id, name: m.name, api: cfg.api });
const patterns = [
	"claude-opus-5-5", "opus", "OPUS-5", "sonnet", "claude-sonnet-5-5:high", "sonnet:low", "haiku", "gpt-4.1", "gpt-4.1:medium",
	"gpt", "mini", "qwen2.5-coder:7b", "qwen2.5-coder:7b:high", "qwen", "llama:xhigh", "foo:high", "foo:high:low", "nothing", "gpt:bogus",
	"anthropic/opus", "openai/gpt", "ollama/qwen2.5-coder:7b:off", "anthropic/gpt", "openai/gpt-4.1:max",
];
let rexp = "";
for (const pat of patterns) {
	// resolveCliModel's provider inference (model-resolver.ts :445-:520) before parseModelPattern
	let provider: string | undefined;
	let p = pat;
	const sl = pat.indexOf("/");
	if (sl !== -1) {
		const mp = all.find((m) => m.provider.toLowerCase() === pat.slice(0, sl).toLowerCase());
		if (mp) { provider = mp.provider; p = pat.slice(sl + 1); }
	}
	const cand = provider ? all.filter((m) => m.provider === provider) : all;
	const r = parseModelPattern(p, cand, { allowInvalidThinkingLevelFallback: false });
	rexp += `${pat}\t${r.model ? r.model.provider + "/" + r.model.id : "-"}\t${r.thinkingLevel ?? "-"}\n`;
}
writeFileSync(join(out, "resolve.patterns"), patterns.join("\n") + "\n");
writeFileSync(join(out, "resolve.expected"), rexp);

// ---- config values ----
const cfgCases: { name: string; value: string; env: Record<string, string> }[] = [
	{ name: "literal", value: "sk-plain", env: {} },
	{ name: "dollar", value: "$PI_T_KEY", env: { PI_T_KEY: "abc" } },
	{ name: "braces", value: "pre-${PI_T_KEY}-post", env: { PI_T_KEY: "abc" } },
	{ name: "two", value: "$PI_T_A:$PI_T_B", env: { PI_T_A: "1", PI_T_B: "2" } },
	{ name: "missing", value: "$PI_T_MISSING", env: {} },
	{ name: "missing_second", value: "$PI_T_A$PI_T_MISSING", env: { PI_T_A: "1" } },
	{ name: "escaped_dollar", value: "cost$$5", env: {} },
	{ name: "escaped_bang", value: "$!notacommand", env: {} },
	{ name: "lone_dollar", value: "a $ b", env: {} },
	{ name: "unclosed_brace", value: "x${PI_T_A", env: { PI_T_A: "1" } },
	{ name: "bad_brace_name", value: "x${1A}y", env: {} },
	{ name: "command", value: "!echo from-command", env: {} },
	{ name: "command_trimmed", value: "!printf '  spaced  \\n\\n'", env: {} },
	{ name: "command_fails", value: "!exit 3", env: {} },
	{ name: "command_empty", value: "!true", env: {} },
];
writeFileSync(join(out, "cfg.cases"), cfgCases.map((c) => JSON.stringify(c)).join("\n") + "\n");
let cexp = "";
for (const c of cfgCases) {
	for (const k of Object.keys(c.env)) process.env[k] = c.env[k];
	const v = resolveConfigValueUncached(c.value, undefined);
	for (const k of Object.keys(c.env)) delete process.env[k];
	cexp += `${c.name}\t${v === undefined ? "UNRESOLVED" : JSON.stringify(v)}\n`;
}
writeFileSync(join(out, "cfg.expected"), cexp);
console.log("[OK] tests/pi/model written");
