// gen_oauth.mjs -- the expected outputs of tests/test_pi_login.c (plan phase O4), produced by pi 1.1.0's own OAuth code:
// pi-ai's login and refresh functions (auth/oauth/openai-chatgpt.js, openai-codex.js, github-copilot.js, kimi-coding.js,
// xai.js, meta.js) run on each scenario of tests/pi/oauth/scenarios.json with a capturing fetch that answers from the
// scenario's responses, a clock that only moves when pi sleeps, a random generator that counts, and a scripted user.
// Writes, per scenario, next to scenarios.json:
//   <name>.req   every request pi sent, in the text tools/claude_fixture.py writes to its requests.log: "METHOD URL", the
//                headers pi set (names in lower case, sorted), a blank line, the body, then "--"
//   <name>.out   what the user was shown and asked, one line each, then the end: "ok <credential as JSON>" or
//                "error <message>"
// The scripted user: a select prompt takes the scenario's next "select" index, a text prompt its next "text", a pasted
// redirect/code is "paste" with $STATE replaced by the state parameter of the URL pi showed.
//   PI_NPM=<dir holding node_modules/@earendil-works/pi-ai> node tools/pi_oracle/gen_oauth.mjs [--only name]
//   (the pinned install: vendor/pi-1.1.0)
import { readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const npm = process.env.PI_NPM || join(here, "..", "..", "vendor", "pi-1.1.0");
const dist = join(npm, "node_modules/@earendil-works/pi-ai/dist");
const dir = join(here, "..", "..", "tests", "pi", "oauth");
const { now: NOW, scenarios } = JSON.parse(readFileSync(join(dir, "scenarios.json"), "utf8"));
const only = process.argv.includes("--only") ? process.argv[process.argv.indexOf("--only") + 1] : null;

// ---- the world pi runs in: a clock that moves when pi sleeps, a random generator that counts ----
let clock = NOW;
Date.now = () => clock;
const realSetTimeout = globalThis.setTimeout;
globalThis.setTimeout = (fn, ms, ...args) => {
	clock += ms;
	return realSetTimeout(fn, 0, ...args);
};
let rk = 0;
const rnd = (n) => Buffer.from(Array.from({ length: n }, () => (rk++ * 37 + 11) & 255));
const nodeCrypto = createRequire(import.meta.url)("node:crypto");
nodeCrypto.randomBytes = rnd;
(await import("node:module")).syncBuiltinESMExports();
Object.defineProperty(globalThis.crypto, "getRandomValues", {
	configurable: true,
	value: (a) => {
		a.set(rnd(a.length));
		return a;
	},
});

// crypto.randomUUID() from the same counting generator (OpenRouter's callback path, Radius' state)
Object.defineProperty(globalThis.crypto, "randomUUID", {
	configurable: true,
	value: () => {
		const b = rnd(16);
		b[6] = (b[6] & 0x0f) | 0x40;
		b[8] = (b[8] & 0x3f) | 0x80;
		const h = b.toString("hex");
		return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
	},
});

const imp = (p) => import(pathToFileURL(join(dist, p)).href);
const mods = {
	openai: (await imp("auth/oauth/openai-chatgpt.js")).openaiChatGPTOAuth,
	"openai-codex": (await imp("auth/oauth/openai-codex.js")).openaiCodexOAuth,
	"github-copilot": (await imp("auth/oauth/github-copilot.js")).githubCopilotOAuth,
	"kimi-coding": (await imp("auth/oauth/kimi-coding.js")).kimiCodingOAuth,
	xai: (await imp("auth/oauth/xai.js")).xaiOAuth,
	meta: (await imp("auth/oauth/meta.js")).metaOAuth,
	openrouter: (await imp("auth/oauth/openrouter.js")).openRouterOAuth,
	radius: (await imp("auth/oauth/radius.js")).createRadiusOAuth({ name: "Radius", gateway: "https://radius.pi.dev" }),
};
const { MODELS } = await imp("models.generated.js");

const STATUS = { 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 408: "Request Timeout", 429: "Too Many Requests", 500: "Internal Server Error", 502: "Bad Gateway", 503: "Service Unavailable", 504: "Gateway Timeout" };

function reqText(method, url, headers, body) {
	const h = Object.entries(headers)
		.map(([k, v]) => [k.toLowerCase(), String(v)])
		.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
	return `${method} ${url}\n${h.map(([k, v]) => `${k}: ${v}`).join("\n")}${h.length ? "\n" : ""}\n${body}\n--\n`;
}

async function run(sc) {
	clock = NOW;
	rk = 0;
	const requests = [];
	const events = [];
	const used = {};
	const by = {};
	for (const r of sc.responses) (by[r.url] ??= []).push(r);
	globalThis.fetch = async (url, init = {}) => {
		url = String(url);
		const headers = init.headers instanceof Headers ? Object.fromEntries(init.headers) : { ...(init.headers || {}) };
		requests.push(reqText(init.method || "GET", url, headers, init.body === undefined ? "" : String(init.body)));
		const rs = by[url];
		if (!rs) return new Response('{"error":"no script for this url"}', { status: 404, statusText: STATUS[404] });
		const r = rs[Math.min(used[url] ?? 0, rs.length - 1)];
		used[url] = (used[url] ?? 0) + 1;
		const body = "json" in r ? JSON.stringify(r.json) : (r.text ?? "");
		const status = r.status ?? 200;
		return new Response(body, { status, statusText: STATUS[status] ?? "", headers: r.headers });
	};
	const ac = new AbortController();
	const selects = [...(sc.select || [])];
	const texts = [...(sc.text || [])];
	let authUrl = "";
	const interaction = {
		signal: ac.signal,
		notify(e) {
			if (e.type === "auth_url") {
				authUrl = e.url;
				events.push(`auth_url ${e.url} | ${e.instructions ?? ""}`);
			} else if (e.type === "device_code") events.push(`device_code ${e.userCode} ${e.verificationUri}`);
			else if (e.type === "info") events.push(`info ${e.message}`);
			else events.push(`progress ${e.message}`);
		},
		async prompt(p) {
			if (p.type === "select") {
				events.push(`prompt select ${p.message} [${p.options.map((o) => o.label).join(" | ")}]`);
				return p.options[selects.shift()].id;
			}
			events.push(`prompt ${p.type === "manual_code" ? "manual" : p.type} ${p.message} | ${p.placeholder ?? ""}`);
			if (p.type === "manual_code") {
				const state = new URL(authUrl).searchParams.get("state") ?? "";
				return sc.paste.replaceAll("$STATE", state);
			}
			return texts.shift() ?? "";
		},
	};
	const oauth = mods[sc.provider];
	let end;
	try {
		const cred =
			sc.op === "login"
				? await oauth.login(interaction, { getDeviceId: () => sc.deviceId })
				: await oauth.refresh(sc.credential, ac.signal);
		end = `ok ${JSON.stringify(cred)}`;
	} catch (e) {
		end = `error ${e instanceof Error ? e.message : String(e)}`;
	}
	// A scenario that listens on a loopback port of pi's choosing (OpenRouter): amiga-pi has no listener and picks its own port, so
	// the port is left out of both sides ("normalizePort"), and pi's line that says it is listening is not part of the golden
	// ("dropListening").
	let shown = events;
	if (sc.dropListening) shown = shown.filter((l) => !l.startsWith("progress Listening for "));
	let out = shown.map((l) => l + "\n").join("") + end + "\n";
	if (sc.normalizePort) out = out.replace(/127\.0\.0\.1(%3A|:)\d+/g, "127.0.0.1$1PORT");
	return { req: requests.join(""), out };
}

let n = 0;
for (const sc of scenarios) {
	if (only && sc.name !== only) continue;
	const r = await run(sc);
	writeFileSync(join(dir, sc.name + ".req"), r.req);
	writeFileSync(join(dir, sc.name + ".out"), r.out);
	n++;
}
// auth.json as pi writes it (JSON.stringify(data, null, 2)): the file after a login, a replaced entry, a logout
// (store_in.json is the compact file the cases start from; the C side must lay the same members out the same way)
{
	const cur = JSON.parse(readFileSync(join(dir, "store_in.json"), "utf8"));
	const kimi = { type: "oauth", access: "k", refresh: "r2", expires: 5 };
	const out = (name, v) => writeFileSync(join(dir, name), JSON.stringify(v, null, 2) + "\n");
	out("store_added.json", { ...cur, "kimi-coding": kimi });
	out("store_replaced.json", { ...cur, openai: { type: "api_key", key: "sk-y" } });
	const { openai, ...rest } = cur;
	out("store_removed.json", rest);
	out("store_empty.json", {});
	out("store_first.json", { "kimi-coding": kimi });
}
console.log(`[OK] tests/pi/oauth written, ${n} scenarios (${Object.keys(MODELS["github-copilot"]).length} copilot models known)`);
process.exit(0);
