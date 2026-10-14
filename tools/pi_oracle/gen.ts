// gen.ts -- the expected outputs of tests/test_pi_session.c, produced by pi's own
// SessionManager, compaction and branch-summarization code: the pinned pi 1.2.0 install
// (vendor/pi-1.2.0, npm install --prefix vendor/pi-1.2.0 @earendil-works/pi-coding-agent@1.2.0; tag v1.2.0
// 06a46693). No copied or patched source: the package's own dist is imported.
//
//   node tools/pi_oracle/gen.ts                    (make oracle-session)
//
// PI_ORACLE_DIR names another node_modules parent (the tests point it at vendor/pi-1.1.0 to prove the pin
// check refuses it). Writes, next to each fixture tests/pi/sessions/<name>.jsonl, <name>.expected (a text
// dump of what opening it gives), and writer.scn + writer.expected (a scenario of appends and the file pi
// writes for it, ids and times normalised). PI_SESSIONS_OUT redirects the writes (the test's reproducibility
// check regenerates into a temp copy and compares).
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const sessions = resolve(here, "..", "..", "tests", "pi", "sessions");
const outDir = process.env.PI_SESSIONS_OUT ?? sessions;
const PINNED = "1.2.0";
// this checkout's vendor/, else the main checkout's when this is build/wt-<name>
function findOracle(): string {
	// exit 2: the install found is not 1.2.0; exit 3: there is none
	if (process.env.PI_ORACLE_DIR) return resolve(process.env.PI_ORACLE_DIR);
	for (const root of ["../..", "../../../.."]) {
		const c = resolve(here, root, "vendor", "pi-1.2.0");
		if (existsSync(join(c, "node_modules", "@earendil-works", "pi-coding-agent", "package.json"))) return c;
	}
	console.error("gen: no pinned pi: npm install --prefix vendor/pi-1.2.0 @earendil-works/pi-coding-agent@1.2.0");
	process.exit(3);
}
const npmRoot = join(findOracle(), "node_modules");
const caDir = join(npmRoot, "@earendil-works", "pi-coding-agent");
const caVersion = JSON.parse(readFileSync(join(caDir, "package.json"), "utf8")).version;
if (caVersion !== PINNED) {
	console.error(`gen: the oracle is pi-coding-agent ${caVersion}, not the pinned ${PINNED}; refusing to write goldens`);
	process.exit(2);
}
const dist = (f: string) => import(pathToFileURL(join(caDir, "dist", f)).href);
const { SessionManager } = await dist("core/session-manager.js");
const { convertToLlm } = await dist("core/messages.js");
const { prepareCompaction, compact, estimateTokens, shouldCompact } = await dist("core/compaction/compaction.js");
const { collectEntriesForBranchSummary, prepareBranchEntries, generateBranchSummary } = await dist("core/compaction/branch-summarization.js");
const { serializeConversation } = await dist("core/compaction/utils.js");
const { getCurrentSystemMessage, getSystemMessageText } = await import(
	pathToFileURL(join(npmRoot, "@earendil-works", "pi-ai", "dist", "index.js")).href);
if (process.env.PI_ORACLE_VERSION_ONLY) process.exit(0);

// ---- the text normaliser (mirrored in tests/test_pi_session.c) ----
// the values of "id", "parentId", "targetId", "firstKeptEntryId", "fromId" become
// i1, i2, ... in order of first appearance; a 36-character header id becomes SID;
// an entry's "timestamp" string becomes T, a 13-digit number N, a parentSession path P.
function normalise(text: string): string {
	const map = new Map<string, string>();
	let out = text.replace(/"timestamp":"[^"]*"/g, '"timestamp":"T"');
	out = out.replace(/"parentSession":"[^"]*"/g, '"parentSession":"P"');
	out = out.replace(/"timestamp":\d{13}/g, '"timestamp":N'); // a compaction's systemMessage carries the clock
	out = out.replace(/"(id|parentId|targetId|firstKeptEntryId|fromId)":"([^"]*)"/g, (_m, k, v) => {
		if (v.length === 36 && k === "id") return `"id":"SID"`;
		if (!map.has(v)) map.set(v, `i${map.size + 1}`);
		return `"${k}":"${map.get(v)}"`;
	});
	return out;
}

// ---- compaction: prepareCompaction and compact() with a fake model ----
// the model answers "SUMMARY n" with usage 100*n input tokens, so the combined usage and the order of
// the calls are visible; the requests are captured (the prompt text is what C must send)
const SUMMARY_USAGE = (n: number) => ({ input: 100 * n, output: 10 * n, cacheRead: n, cacheWrite: 2 * n, totalTokens: 113 * n,
	cost: { input: 0.5 * n, output: 0.25 * n, cacheRead: 0, cacheWrite: 0, total: 0.75 * n } });
async function compactionDump(sm: any, keep: number): Promise<string> {
	const settings = { enabled: true, reserveTokens: 16384, keepRecentTokens: keep };
	const prep = prepareCompaction(sm.getBranch(), settings);
	let out = `prepare keep=${keep} `;
	if (!prep) return out + "none\n";
	const { settings: _s, fileOps, ...rest } = prep;
	const fo = { read: [...fileOps.read], written: [...fileOps.written], edited: [...fileOps.edited] };
	out += JSON.stringify({ ...rest, fileOps: fo }) + "\n";
	const reqs: any[] = [];
	let n = 0;
	const streamFn = async (_m: any, ctx: any, opts: any) => {
		n++;
		reqs.push({ system: ctx.messages[0].content, prompt: ctx.messages[1].content[0].text, maxTokens: opts.maxTokens });
		return { result: async () => ({ role: "assistant", content: [{ type: "text", text: `SUMMARY ${n}` }],
			usage: SUMMARY_USAGE(n), stopReason: "stop", timestamp: 1 }) };
	};
	const model = { id: "m", provider: "p", maxTokens: 20000, reasoning: false, contextWindow: 200000 };
	const r = await compact(prep, model, undefined, undefined, "focus on tests", undefined, undefined, streamFn);
	out += `compact keep=${keep} ${JSON.stringify(r)}\n`;
	for (const q of reqs) out += `request maxTokens=${q.maxTokens} system=${JSON.stringify(q.system)} prompt=${JSON.stringify(q.prompt)}\n`;
	return out;
}
async function failureDump(sm: any): Promise<string> {
	const prep = prepareCompaction(sm.getBranch(), { enabled: true, reserveTokens: 16384, keepRecentTokens: 5 });
	if (!prep) return "";
	let out = "";
	for (const stop of ["error", "length"]) {
		const streamFn = async () => ({ result: async () => ({ role: "assistant", content: [{ type: "text", text: "partial" }],
			usage: SUMMARY_USAGE(1), stopReason: stop, errorMessage: stop === "error" ? "boom" : undefined, timestamp: 1 }) });
		try {
			await compact(prep, { id: "m", provider: "p", maxTokens: 100, reasoning: false }, undefined, undefined, undefined, undefined, undefined, streamFn);
			out += `failure ${stop} none\n`;
		} catch (e: any) { out += `failure ${stop} ${e.message}\n`; }
	}
	return out;
}

// ---- branch summaries (/tree with a summary of the branch left) ----
// targets: the first entry, the middle one, and the first two entries that are not on the leaf's branch
async function branchDump(sm: any): Promise<string> {
	const es = sm.getEntries();
	if (es.length === 0) return "";
	const onPath = new Set(sm.getBranch().map((e: any) => e.id));
	const targets: string[] = [es[0].id];
	if (es.length > 1) targets.push(es[Math.floor(es.length / 2)].id);
	for (const e of es) if (!onPath.has(e.id) && targets.length < 4) targets.push(e.id);
	let out = "";
	let n = 0;
	for (const t of targets) {
		const { entries, commonAncestorId } = collectEntriesForBranchSummary(sm, sm.getLeafId(), t);
		out += `branch target=${t} ancestor=${commonAncestorId ?? "-"} entries=${entries.map((e: any) => e.id).join(",")}\n`;
		for (const budget of [0, 40]) {
			const prep = prepareBranchEntries(entries, budget);
			const fo = { read: [...prep.fileOps.read], written: [...prep.fileOps.written], edited: [...prep.fileOps.edited] };
			out += `branchprep budget=${budget} ${JSON.stringify({ messages: prep.messages, fileOps: fo, totalTokens: prep.totalTokens })}\n`;
		}
		const reqs: any[] = [];
		const streamFn = async (_m: any, ctx: any, opts: any) => {
			reqs.push({ system: ctx.messages[0].content, prompt: ctx.messages[1].content[0].text, maxTokens: opts.maxTokens });
			return { result: async () => ({ role: "assistant", content: [{ type: "text", text: "BRANCH SUMMARY" }],
				usage: SUMMARY_USAGE(1), stopReason: "stop", timestamp: 1 }) };
		};
		const instr = n === 1 ? { customInstructions: "focus on tests" } : n === 2 ? { customInstructions: "Only say hi", replaceInstructions: true } : {};
		const model = { id: "m", provider: "p", maxTokens: 20000, reasoning: false, contextWindow: n === 1 ? 16384 + 40 : undefined };
		const r = await generateBranchSummary(entries, { model, signal: undefined, streamFn, ...instr });
		out += `branchsum ${JSON.stringify(r)}\n`;
		for (const q of reqs) out += `request maxTokens=${q.maxTokens} system=${JSON.stringify(q.system)} prompt=${JSON.stringify(q.prompt)}\n`;
		n++;
	}
	return out;
}

// ---- opening a fixture ----
async function dump(src: string, norm: boolean): Promise<string> {
	const dir = mkdtempSync(join(tmpdir(), "pigen-"));
	const path = join(dir, "s.jsonl");
	copyFileSync(src, path);
	const before = statSync(path).size;
	let out = "";
	let sm: any = null;
	try {
		sm = SessionManager.open(path);
	} catch {
		out += "open error\n";
	}
	const ids = new Map<string, string>();
	const mp = (id: string | null | undefined): string => {
		if (id == null) return "-";
		if (!norm) return id;
		if (!ids.has(id)) ids.set(id, `i${ids.size + 1}`);
		return ids.get(id)!;
	};
	if (sm) {
		const h = sm.getHeader();
		out += "open ok\n";
		out += `version ${h.version}\ncwd ${h.cwd}\nparent ${h.parentSession ?? "-"}\n`;
		const es = sm.getEntries();
		out += `entries ${es.length}\n`;
		for (const e of es) {
			let l = `${mp(e.id)} ${mp(e.parentId)} ${e.type}`;
			if (e.type === "label" || e.type === "context_edit") l += ` target=${mp(e.targetId)}`;
			if (e.type === "compaction") l += ` kept=${mp(e.firstKeptEntryId)}`;
			if (e.type === "branch_summary") l += ` from=${mp(e.fromId)}`;
			out += l + "\n";
		}
		out += `leaf ${mp(sm.getLeafId())}\n`;
		for (const e of es) {
			const lb = sm.getLabel(e.id);
			if (lb !== undefined) out += `label ${mp(e.id)} ${lb}\n`;
		}
		out += `name ${sm.getSessionName() ?? "-"}\n`;
		out += `branch ${sm.getBranch().map((e: any) => mp(e.id)).join(",")}\n`;
		// what the model sees (E3): buildSessionContext, convertToLlm, the replayed system message
		if (!norm) {
			const ctx = sm.buildSessionContext();
			out += `context ${JSON.stringify(ctx)}\n`;
			out += `llm ${JSON.stringify(convertToLlm(ctx.messages))}\n`;
			const sys = getCurrentSystemMessage(ctx.messages);
			out += `system ${sys === undefined ? "-" : JSON.stringify(sys)}\n`;
			out += `systemtext ${sys === undefined ? "-" : JSON.stringify(getSystemMessageText(sys))}\n`;
			for (const k of [20000, 5000, 5]) out += await compactionDump(sm, k);
			out += await failureDump(sm);
			// pi throws on a message without content (bad_content.jsonl): no case to match
			if (!/bad_content/.test(src)) out += await branchDump(sm);
			// estimateTokens of every context message, shouldCompact at the edges
			out += `tokens ${JSON.stringify(ctx.messages.map((m: any) => estimateTokens(m)))}\n`;
		}
	}
	let file = readFileSync(path, "utf8");
	if (norm) file = normalise(file);
	out += `size ${before === statSync(path).size ? "same" : "changed"}\nfile\n${file}`;
	return out;
}

// --dump <file>: pi's SessionManager opens a file (one a C build wrote) and prints what it sees;
// exit status 1 when pi refuses it or does not see every line as an entry
const di = process.argv.indexOf("--dump");
if (di > 0) {
	const f = process.argv[di + 1];
	const d = await dump(f, false);
	const lines = readFileSync(f, "utf8").split("\n").filter((l) => l.trim()).length;
	const m = /^entries (\d+)$/m.exec(d);
	console.log(d.split("\nfile\n")[0]);
	process.exit(d.startsWith("open ok") && m && Number(m[1]) === lines - 1 ? 0 : 1);
}

for (const f of readdirSync(sessions).filter((n) => n.endsWith(".jsonl")).sort()) {
	const name = f.replace(/\.jsonl$/, "");
	writeFileSync(join(outDir, name + ".expected"), await dump(join(sessions, f), name === "mig_v1"));
}

// ---- the writer scenario ----
const USAGE = { input: 10, output: 5, cacheRead: 1, cacheWrite: 2, totalTokens: 18,
	cost: { input: 0.1, output: 0.2, cacheRead: 0.01, cacheWrite: 0.02, total: 0.33 } };
const user = (t: number, c: unknown) => ({ role: "user", content: c, timestamp: t });
const asst = (t: number, text: string) => ({ role: "assistant", content: [{ type: "text", text }], api: "anthropic-messages",
	provider: "anthropic", model: "claude-sonnet-4-5", usage: USAGE, stopReason: "stop", timestamp: t });
const tres = (t: number) => ({ role: "toolResult", toolCallId: "call_1", toolName: "bash",
	content: [{ type: "text", text: "out" }], isError: false, timestamp: t });
// refs are 1-based ordinals of the entries appended so far
const scn: any[] = [
	{ op: "message", message: { role: "system", content: "", sections: { preamble: "You are." }, toolsAdded: [], timestamp: 1000 } },
	{ op: "model", provider: "anthropic", modelId: "claude-sonnet-4-5" },
	{ op: "thinking", level: "high" },
	{ op: "message", message: user(1001, "Hello é \"quoted\"\n") },
	{ op: "message", message: asst(1002, "Hi!") },
	{ op: "message", message: tres(1003) },
	{ op: "usage", kind: "cache_warm", provider: "anthropic", model: "claude-sonnet-4-5", usage: USAGE, note: "warm" },
	{ op: "usage", kind: "summary", provider: "openai", model: "gpt-4o", usage: USAGE },
	{ op: "custom", customType: "my-ext", data: { count: 42 } },
	{ op: "custom", customType: "bare" },
	{ op: "custom_message", customType: "my-ext", content: "Injected", display: true },
	{ op: "custom_message", customType: "my-ext", content: [{ type: "text", text: "arr" }], display: false, details: { k: 1 } },
	{ op: "context_edit", target: 4, replacement: { content: "edited user" } },
	{ op: "context_edit", target: 5, replacement: { content: "edited assistant as string" } },
	{ op: "context_edit", target: 6, replacement: null },
	{ op: "label", target: 4, label: "mark" },
	{ op: "label", target: 4, label: null },
	{ op: "session_info", name: "  my\nname \r\n here  " },
	{ op: "compaction", summary: "S", first: 4, tokens: 1234, details: { readFiles: ["a"], modifiedFiles: [] }, usage: USAGE, fromHook: false },
	{ op: "compaction", summary: "Retain none", first: null, tokens: 5 },
	{ op: "message", message: user(1004, "after") },
	{ op: "branch", to: 5 },
	{ op: "branch_summary", to: 5, summary: "went elsewhere", details: { readFiles: [], modifiedFiles: ["b"] }, usage: USAGE, fromHook: true },
	{ op: "message", message: user(1005, [{ type: "text", text: "blocks" }]) },
	{ op: "branch", to: null },
	{ op: "message", message: user(1006, "second root") },
];
function runScenario(scn: any[]): string {
	const dir = mkdtempSync(join(tmpdir(), "pigenw-"));
	const sm: any = SessionManager.create("/work/dir", dir);
	const ids: string[] = [];
	const ref = (n: number | null) => (n === null ? null : ids[n - 1]);
	for (const s of scn) {
		let id: string | undefined;
		switch (s.op) {
			case "message": id = sm.appendMessage(s.message); break;
			case "thinking": id = sm.appendThinkingLevelChange(s.level); break;
			case "model": id = sm.appendModelChange(s.provider, s.modelId); break;
			case "usage": id = sm.appendUsage(s.kind, s.provider, s.model, s.usage, s.note).id; break;
			case "compaction": id = sm.appendCompaction(s.summary, ref(s.first), s.tokens, s.details, s.fromHook, s.usage); break;
			case "custom": id = sm.appendCustomEntry(s.customType, s.data); break;
			case "custom_message": id = sm.appendCustomMessageEntry(s.customType, s.content, s.display, s.details); break;
			case "context_edit": id = sm.appendContextEdit(ref(s.target), s.replacement); break;
			case "label": id = sm.appendLabelChange(ref(s.target), s.label ?? undefined); break;
			case "session_info": id = sm.appendSessionInfo(s.name); break;
			case "branch": if (s.to === null) sm.resetLeaf(); else sm.branch(ref(s.to)); break;
			case "branched": sm.createBranchedSession(ref(s.leaf)); break;
			case "branch_summary": id = sm.branchWithSummary(ref(s.to), s.summary, s.details, s.fromHook, s.usage); break;
		}
		if (id !== undefined) ids.push(id);
	}
	const f = sm.getSessionFile();
	return normalise(readFileSync(f, "utf8"));
}
// createBranchedSession: labels re-chained after the path, a compaction's firstKeptEntryId remapped
// when it names a label entry, parentSession the old file
const branched: any[] = [
	{ op: "message", message: user(2001, "q1") },
	{ op: "message", message: asst(2002, "a1") },
	{ op: "label", target: 1, label: "L1" },
	{ op: "label", target: 2, label: "L2" },
	{ op: "message", message: user(2003, "q2") },
	{ op: "message", message: asst(2004, "a2") },
	{ op: "compaction", summary: "S", first: 4, tokens: 99 },
	{ op: "message", message: user(2005, "q3 dropped") },
	{ op: "branch", to: 7 },
	{ op: "label", target: 5, label: "L3" },
	{ op: "message", message: user(2006, "q3 kept") },
	{ op: "branched", leaf: 10 },
];
for (const [name, list] of [["writer", scn], ["branched", branched]] as [string, any[]][]) {
	writeFileSync(join(outDir, name + ".scn"), list.map((s) => JSON.stringify(s)).join("\n") + "\n");
	writeFileSync(join(outDir, name + ".expected"), runScenario(list));
}
// ---- automatic compaction through pi's own AgentSession (agent-session.ts, agent-loop.ts) ----
// Where pi checks the context against the window: prompt() before the user message (the last answer's usage),
// prepareNextTurn between turns, _handlePostAgentRun after agent_end. These live in AgentSession, which needs
// pi's whole package: the pinned install above.
// The model is a provider answering every request with tests/pi/openai_text.sse's text and usage, as the C
// side's stub does. Two runs over one file, as tests/test_pi_store.c's: "hello" with a large window, then -c
// with a 400-character prompt and a window whose threshold (window - reserveTokens) is 100 tokens. Written:
// agent_compaction.expected -- the requests of each run (answer / summary) and the file's user, assistant and
// compaction entries in order, a compaction's parentId and firstKeptEntryId as ordinals of that list.
{
	const npm = npmRoot;
	const ai = await import(pathToFileURL(join(npm, "@earendil-works/pi-ai/dist/index.js")).href);
	const ca = await import(pathToFileURL(join(npm, "@earendil-works/pi-coding-agent/dist/index.js")).href);
	const RealDate = Date;
	let clock = 1760100000000;
	// one second per reading of the clock: an answer and a compaction never share a millisecond
	class StepDate extends RealDate {
		constructor(...a: any[]) {
			if (a.length === 0) {
				clock += 1000;
				super(clock);
			} else super(...(a as []));
		}
		static now() {
			clock += 1000;
			return clock;
		}
	}
	(globalThis as any).Date = StepDate;
	const usage = { input: 100, output: 9, cacheRead: 20, cacheWrite: 0, totalTokens: 129,
		cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
	let kinds: string[] = [];
	let window = 128000;
	const stream = (m: any, context: any) => {
		const out = ai.createAssistantMessageEventStream();
		kinds.push(JSON.stringify(context).includes("context summarization assistant") ? "summary" : "answer");
		const msg = { role: "assistant", content: [{ type: "text", text: "Hello from the Amiga! Grüße — ok" }],
			api: m.api, provider: m.provider, model: m.id, usage: structuredClone(usage), stopReason: "stop", timestamp: Date.now() };
		queueMicrotask(() => {
			out.push({ type: "start", partial: { ...msg, content: [], stopReason: "pending" } });
			out.push({ type: "done", reason: "stop", message: msg });
			out.end(msg);
		});
		return out;
	};
	const dir = mkdtempSync(join(tmpdir(), "piagent"));
	const runs: string[] = [];
	const run = async (sm: any, prompt: string) => {
		const rt = await ca.ModelRuntime.create({ modelsPath: null, authPath: join(dir, "auth.json") });
		rt.registerNativeProvider(ai.createProvider({ id: "fixture", auth: { apiKey: { name: "Fixture", resolve: async () => ({ auth: {} }) } },
			models: [{ id: "m", name: "m", api: "fixture-api", provider: "fixture", baseUrl: "http://x", reasoning: false, input: ["text"],
				cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 }, contextWindow: window, maxTokens: 4096 }],
			api: { stream, streamSimple: stream } }));
		const { session } = await ca.createAgentSession({ cwd: dir, agentDir: dir, modelRuntime: rt, model: rt.getModel("fixture", "m"),
			sessionManager: sm, settingsManager: ca.SettingsManager.inMemory({ compaction: { keepRecentTokens: 5 } }), noTools: "all" });
		kinds = [];
		await session.prompt(prompt);
		runs.push(`run ${runs.length + 1}: ${kinds.join(" ")}`);
	};
	const sm1 = ca.SessionManager.create(dir, join(dir, "sessions"));
	await run(sm1, "hello");
	const file = sm1.getSessionFile();
	window = 16384 + 100;
	let long = "";
	for (let i = 0; i < 400; i++) long += String.fromCharCode(97 + (i % 26));
	await run(ca.SessionManager.open(file), long);
	(globalThis as any).Date = RealDate;
	const listed: any[] = [];
	for (const line of readFileSync(file, "utf8").split("\n")) {
		if (!line.trim()) continue;
		const e = JSON.parse(line);
		if (e.type === "compaction" || (e.type === "message" && e.message.role !== "system")) listed.push(e);
	}
	const ord = (id: string) => listed.findIndex((e) => e.id === id) + 1;
	const lines = [...runs];
	for (const e of listed)
		lines.push(e.type === "compaction"
			? `compaction parent=${ord(e.parentId)} firstKept=${ord(e.firstKeptEntryId)} tokensBefore=${e.tokensBefore}`
			: e.message.role);
	writeFileSync(join(outDir, "agent_compaction.expected"), lines.join("\n") + "\n");
}

console.log("gen: " + readdirSync(outDir).filter((n) => n.endsWith(".expected")).length + " expected files");
