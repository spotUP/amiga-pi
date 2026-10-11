// gen.ts -- the expected outputs of tests/test_pi_session.c, produced by pi's own
// SessionManager (pi f10993bc, packages/coding-agent/src/core/session-manager.ts).
//
//   PI_ORACLE=<dir> node tools/pi_oracle/gen.ts        (tools/pi_oracle/gen.sh)
//
// PI_ORACLE is the oracle tree: pi's session-manager.ts and compaction/ files with
// config, paths and pi-ai stubbed (see tools/pi_oracle/README in gen.sh). Writes,
// next to each fixture tests/pi/sessions/<name>.jsonl, <name>.expected (a text dump
// of what opening it gives), and writer.scn + writer.expected (a scenario of appends
// and the file pi writes for it, ids and times normalised).
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const sessions = resolve(here, "..", "..", "tests", "pi", "sessions");
const oracle = process.env.PI_ORACLE ?? join(process.env.HOME!, "Code/amiga-pi/build/pi-oracle/oracle");
const { SessionManager } = await import(pathToFileURL(join(oracle, "core/session-manager.ts")).href);

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

// ---- opening a fixture ----
function dump(src: string, norm: boolean): string {
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
	const d = dump(f, false);
	const lines = readFileSync(f, "utf8").split("\n").filter((l) => l.trim()).length;
	const m = /^entries (\d+)$/m.exec(d);
	console.log(d.split("\nfile\n")[0]);
	process.exit(d.startsWith("open ok") && m && Number(m[1]) === lines - 1 ? 0 : 1);
}

for (const f of readdirSync(sessions).filter((n) => n.endsWith(".jsonl")).sort()) {
	const name = f.replace(/\.jsonl$/, "");
	writeFileSync(join(sessions, name + ".expected"), dump(join(sessions, f), name === "mig_v1"));
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
	writeFileSync(join(sessions, name + ".scn"), list.map((s) => JSON.stringify(s)).join("\n") + "\n");
	writeFileSync(join(sessions, name + ".expected"), runScenario(list));
}
console.log("gen: " + readdirSync(sessions).filter((n) => n.endsWith(".expected")).length + " expected files");
