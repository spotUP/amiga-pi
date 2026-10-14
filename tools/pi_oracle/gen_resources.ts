// gen_resources.ts -- the expected outputs of tests/test_pi_resources.c (plan phase G), produced by the pinned
// pi 1.2.0 install's own code (tools/pi_oracle/pin.mjs): loadProjectContextFiles and DefaultResourceLoader.reload
// (SYSTEM.md / APPEND_SYSTEM.md discovery, resolvePromptInput), buildSystemPromptSections, ProjectTrustStore,
// getProjectTrustOptions and hasTrustRequiringProjectResources. Nothing is cut or copied.
//
//   node tools/pi_oracle/gen_resources.ts
//
// The trees below are made in a temp directory; every path in the output has that directory
// replaced by @ROOT@ (tests/test_pi_resources.c makes the same trees and does the same).
// Writes tests/pi/resources/<layout>.json: the prompt's sections in order ({name: text}), and
// trust.json (the store's file after a series of updates) and trust.txt (labels, needed checks).
import { mkdirSync, mkdtempSync, realpathSync, rmSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { caDir } from "./pin.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const outDir = resolve(here, "..", "..", "tests", "pi", "resources");
const dist = (f: string) => import(pathToFileURL(join(caDir, "dist", f)).href);
const { buildSystemPromptSections } = await dist("core/system-prompt.js");
const R = await dist("core/resource-loader.js");
const T = await dist("core/trust-manager.js");
const { SettingsManager } = await dist("core/settings-manager.js");

const root = realpathSync(mkdtempSync(join(tmpdir(), "pi_res_")));
// the docs paths name the install (dirname of the package); the C side has "/pi"
const norm = (s: string) => s.split(root).join("@ROOT@").split(caDir).join("/pi");
function put(rel: string, text: string) {
	const p = join(root, rel);
	mkdirSync(dirname(p), { recursive: true });
	writeFileSync(p, text);
}
mkdirSync(outDir, { recursive: true });

// the same tools as the C test: two with snippets, no guidelines
const tools = { selectedTools: ["read", "bash"], toolSnippets: { read: "Read file contents", bash: "Execute bash commands" } };

// pi's _rebuildSystemPrompt over the resource loader's discoveries
async function prompt(cwd: string, agentDir: string, trusted: boolean) {
	// the real loader, reloaded: context files, SYSTEM.md and APPEND_SYSTEM.md as pi's own reload() discovers them
	const settingsManager = SettingsManager.inMemory({});
	settingsManager.isProjectTrusted = () => trusted;
	const loader = new R.DefaultResourceLoader({ cwd, agentDir, settingsManager, noExtensions: true, noSkills: true,
		noPromptTemplates: true, noThemes: true });
	await loader.reload();
	const app = loader.getAppendSystemPrompt();
	return buildSystemPromptSections({ cwd, contextFiles: loader.getAgentsFiles().agentsFiles, customPrompt: loader.getSystemPrompt(),
		appendSystemPrompt: app.length > 0 ? app.join("\n\n") : "", ...tools });
}
async function layout(name: string, cwd: string, agent: string, trusted: boolean) {
	mkdirSync(join(root, cwd), { recursive: true });
	mkdirSync(join(root, agent), { recursive: true });
	const s = await prompt(join(root, cwd), join(root, agent), trusted);
	writeFileSync(join(outDir, name + ".json"), norm(JSON.stringify(s, null, 1)) + "\n");
}

// 1. the default prompt; context files in the agent dir and at two levels (root-most first); the
//    agent dir's file first; a BOM dropped; CLAUDE.md where there is no AGENTS.md
put("l1/agent/AGENTS.md", "AGENT-WIDE\n");
put("l1/w/AGENTS.md", "﻿OUTER rules\n");
put("l1/w/proj/CLAUDE.md", "INNER rules\nline two");
await layout("default", "l1/w/proj", "l1/agent", false);

// 2. the agent dir's SYSTEM.md and APPEND_SYSTEM.md (no project .pi)
put("l2/agent/SYSTEM.md", "You are AGENT-SYSTEM.");
put("l2/agent/APPEND_SYSTEM.md", "AGENT-APPEND\n");
put("l2/w/proj/AGENTS.md", "P2\n");
await layout("agent_system", "l2/w/proj", "l2/agent", false);

// 3. project .pi/ files and the agent dir's, trusted: the project's win; AGENTS.override.md over AGENTS.md
put("l3/agent/SYSTEM.md", "AGENT-SYSTEM");
put("l3/agent/APPEND_SYSTEM.md", "AGENT-APPEND");
put("l3/w/proj/.pi/SYSTEM.md", "PROJECT-SYSTEM");
put("l3/w/proj/.pi/APPEND_SYSTEM.md", "PROJECT-APPEND");
put("l3/w/proj/AGENTS.override.md", "OVERRIDE");
put("l3/w/proj/AGENTS.md", "NOT-READ");
await layout("project_trusted", "l3/w/proj", "l3/agent", true);

// 4. the same tree, not trusted: the agent dir's
await layout("project_untrusted", "l3/w/proj", "l3/agent", false);

// 5. only APPEND_SYSTEM.md in the project, trusted, none in the agent dir: default prefix + addendum
put("l5/w/proj/.pi/APPEND_SYSTEM.md", "ONLY-APPEND");
await layout("append_only", "l5/w/proj", "l5/agent", true);

// trust: the store's file after pi's own updates (setMany as the options do)
const agentT = join(root, "trustagent");
const store = new T.ProjectTrustStore(agentT);
store.set("/w/proj/a", true);
store.setMany(T.getProjectTrustOptions("/w/proj/a")[1].updates); // "Trust parent folder (/w/proj)"
store.set("/w/b", false);
store.set("/w/Zeta", true);
store.set("/w/b/c", true);
store.set("/w/b/c", null);
writeFileSync(join(outDir, "trust.json"), readFileSync(join(agentT, "trust.json"), "utf-8"));
const lines: string[] = [];
lines.push("get /w/proj/a/x " + store.get("/w/proj/a/x"));
lines.push("get /w/b/c " + store.get("/w/b/c"));
lines.push("get /q " + store.get("/q"));
for (const o of T.getProjectTrustOptions(join(root, "l3/w/proj"), { includeSessionOnly: true })) lines.push("option " + norm(o.label) + " " + o.trusted);
for (const [rel, what] of [["l1/w/proj", ""], ["l3/w/proj", ""], ["n1/p", ".pi/skills/"], ["n2/p", ".pi/"], ["n3/p", "../.agents/skills/"]] as const) {
	if (what) mkdirSync(join(root, rel, what), { recursive: true });
	else mkdirSync(join(root, rel), { recursive: true });
	lines.push("needed " + rel + " " + T.hasTrustRequiringProjectResources(join(root, rel)));
}
writeFileSync(join(outDir, "trust.txt"), lines.join("\n") + "\n");
rmSync(root, { recursive: true, force: true });
console.log("wrote", outDir);
