// gen_skills.mjs -- the expected outputs of tests/test_pi_skills.c (plan phase I), produced by pi 1.2.0's own code: the pinned install
// vendor/pi-1.2.0 (tools/pi_oracle/pin.mjs), never the pi on the PATH. What runs is pi's
// DefaultResourceLoader (core/resource-loader.ts, core/package-manager.ts: the order and the places skills are found in, settings.json "skills"),
// loadSkills (core/skills.ts: the validation and its messages), buildSystemPromptSections (the "skills" section) and
// AgentSession._expandSkillCommand (core/agent-session.ts: /skill:name args).
//
//   node tools/pi_oracle/gen_skills.mjs          writes tests/pi/skills/layouts.json and <layout>.json
//
// The trees of every layout are described once, here, and written to layouts.json; tests/test_pi_skills.c reads layouts.json, makes the
// same trees in its own temp directory and compares what pi_skills_load gives with <layout>.json. Paths in the output have the
// layout's directory replaced by @ROOT@. pi's ~/.agents/skills is <layout>/home/.agents/skills here and <layout>/agent/agents-skills in
// amiga-pi (the Amiga has no home): the output names it the amiga-pi way.
import { mkdirSync, mkdtempSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { caDir } from "./pin.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const outDir = resolve(here, "..", "..", "tests", "pi", "skills");
process.env.PI_OFFLINE = "1"; // a package source npm:/git: is not fetched: pi skips it
const pkg = caDir;
const pi = await import(pathToFileURL(join(pkg, "dist", "index.js")).href);
const { buildSystemPromptSections } = await import(pathToFileURL(join(pkg, "dist", "core", "system-prompt.js")).href);
const { AgentSession } = await import(pathToFileURL(join(pkg, "dist", "core", "agent-session.js")).href);
const { resolvePath: piResolvePath } = await import(pathToFileURL(join(pkg, "dist", "utils", "paths.js")).href);
const version = JSON.parse((await import("node:fs")).readFileSync(join(pkg, "package.json"), "utf-8")).version;

const sk = (name, desc, extra = "") =>
	`---\n${name === null ? "" : `name: ${name}\n`}${desc === null ? "" : `description: ${desc}\n`}${extra}---\nBody of ${name ?? "the skill"}.\n\nSecond paragraph.\n`;
const rep = (c, n) => c.repeat(n);

// layout: files {path relative to the layout dir: text}, cwd, trusted, noSkills, skillPaths (relative to cwd), expand (inputs of /skill:)
const L = {};
L.basic = {
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/skills/alpha/SKILL.md": sk("alpha", `Alpha does <things> & "more" it's 'x'`),
		"agent/skills/beta/SKILL.md": sk("beta", "Beta is hidden from the model", "disable-model-invocation: true\n"),
		"agent/skills/gamma.md": sk(null, "A loose file in the skills directory"),
		"agent/skills/notes.md": "# Notes\n\nNo frontmatter here.\n",
		"agent/agents-skills/ag/SKILL.md": sk("ag", "From the user's agents directory"),
		"w/proj/.pi/skills/proj-skill/SKILL.md": sk("proj-skill", "Project skill"),
		"w/proj/.pi/skills/nested/deep/SKILL.md": sk("deep", "Found below a plain directory"),
		"w/proj/.agents/skills/pa/SKILL.md": sk("pa", "From the project's .agents/skills"),
	},
	expand: ["/skill:alpha", "/skill:alpha  do the thing  ", "/skill:alpha line one\nline two", "/skill:nope x", "/skill:beta use me", "/skill:alpha\nfoo", "plain text", "/skill:proj-skill  a  b "],
	tools: [["read", "bash"], ["bash"], ["grep"]],
};
// the reachability test (pi_app -> pi_prompt -> the request body): two skills the model sees, one it does not
L.reach = {
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/alpha/SKILL.md": sk("alpha", "Alpha does the first thing"),
		"agent/skills/zeta/SKILL.md": sk("zeta", "Zeta does the other thing"),
		"agent/skills/hid/SKILL.md": sk("hid", "Only /skill:hid loads this one", "disable-model-invocation: true\n"),
	},
	expand: ["/skill:alpha review it", "/skill:hid"],
	tools: [["read", "bash"]],
};
L.untrusted = { ...L.basic, trusted: false, expand: ["/skill:proj-skill", "/skill:alpha x"] };
L.validation = {
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/toolong/SKILL.md": sk(rep("a", 65), "Name too long"),
		"agent/skills/exact64/SKILL.md": sk(rep("b", 64), rep("d", 1024)),
		"agent/skills/upper/SKILL.md": sk("Upper_Case", "Bad characters"),
		"agent/skills/lead/SKILL.md": sk("-lead", "Leading hyphen"),
		"agent/skills/tail/SKILL.md": sk("tail-", "Trailing hyphen"),
		"agent/skills/dbl/SKILL.md": sk("dbl--hy", "Consecutive hyphens"),
		"agent/skills/nodesc/SKILL.md": sk("nodesc", null),
		"agent/skills/blankdesc/SKILL.md": sk("blankdesc", "'   '"),
		"agent/skills/longdesc/SKILL.md": sk("longdesc", rep("d", 1025)),
		"agent/skills/dirname-only/SKILL.md": sk(null, "Name from the directory"),
		"agent/skills/numname/SKILL.md": sk("123", "A number is not a string"),
		"agent/skills/bare/SKILL.md": "No frontmatter at all.\n",
		"agent/skills/colon/SKILL.md": "---\nname: colon\ndescription: Use when: something happens\n---\nBody\n",
		"agent/skills/folded/SKILL.md": "---\nname: folded\ndescription: >-\n  First line\n  second line\n\n  after a gap\nlicense: MIT\n---\nBody\n",
		"agent/skills/literal/SKILL.md": "---\nname: literal\ndescription: |\n  Line one\n  Line two\n---\nBody\n",
		"agent/skills/quoted/SKILL.md": '---\nname: quoted\ndescription: "Tab\\there \\u00e9 \\"q\\" and \\\\"\n---\nBody\n',
		"agent/skills/single/SKILL.md": "---\nname: single\ndescription: 'It''s single'\n---\nBody\n",
		"agent/skills/crlf/SKILL.md": "﻿---\r\nname: crlf\r\ndescription: Windows line endings\r\n---\r\nBody\r\n",
		"agent/skills/multiline/SKILL.md": "---\nname: multiline\ndescription: one\n  two\n  three\n---\nBody\n",
		"agent/skills/bothbad/SKILL.md": sk("Bad Name", null),
		"agent/skills/nomodel-str/SKILL.md": sk("nomodel-str", "A string true is not true", 'disable-model-invocation: "true"\n'),
		"agent/skills/nomodel-caps/SKILL.md": sk("nomodel-caps", "Hidden by True", "disable-model-invocation: True\n"),
		"agent/skills/comment/SKILL.md": "---\nname: comment\ndescription: Before # the comment\nmetadata:\n  author: me\n  version: 1\nallowed-tools: Read Grep\n---\nBody\n",
		"agent/skills/unicode/SKILL.md": sk("unicode", "café 😀 emoji counts two"),
		"agent/skills/bad-yaml-root.md": "---\ndescription: Use when: this\n---\nA root file with broken YAML is skipped without a word.\n",
	},
	expand: ["/skill:folded", "/skill:crlf x", "/skill:bare"],
	tools: [["read"]],
};
L.collision = {
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/skills/dup/SKILL.md": sk("dup", "From the agent directory"),
		"w/proj/.pi/skills/dup/SKILL.md": sk("dup", "From the project"),
		"agent/skills/other/SKILL.md": sk("dup", "Named dup by its frontmatter"),
		"agent/skills/zed/SKILL.md": sk("zed", "No clash"),
	},
	expand: ["/skill:dup"],
	tools: [["read"]],
};
L.agents_walk = {
	cwd: "repo/pkg/sub",
	trusted: true,
	files: {
		"repo/.git/HEAD": "ref: refs/heads/main\n",
		"repo/.agents/skills/a/SKILL.md": sk("a", "At the repository top"),
		"repo/pkg/.agents/skills/b/SKILL.md": sk("b", "In the package"),
		"repo/pkg/.agents/skills/loose.md": sk("loose", "A loose file at the root of .agents/skills is not a skill"),
		"repo/pkg/.agents/skills/c/extra.md": sk(null, "A markdown file below the root is"),
		"repo/pkg/sub/.agents/skills/s/SKILL.md": sk("s", "In the working directory"),
		".agents/skills/up/SKILL.md": sk("up", "Above the repository: not searched"),
		"agent/skills/u/SKILL.md": sk("u", "User skill"),
	},
	expand: [],
	tools: [["read"]],
};
L.agents_nogit = {
	cwd: "w/p/q",
	trusted: true,
	files: {
		"w/.agents/skills/two/SKILL.md": sk("two", "Two levels up"),
		"w/p/q/.agents/skills/here/SKILL.md": sk("here", "Here"),
		".agents/skills/top/SKILL.md": sk("top", "At the layout's top"),
	},
	expand: [],
	tools: [["read"]],
};
L.settings = {
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/settings.json": JSON.stringify({ skills: ["extra", "!skip-*", "-keep-out"], enableSkillCommands: false }),
		"agent/extra/e1/SKILL.md": sk("e1", "From the settings path"),
		"agent/extra/e2/SKILL.md": sk("e2", "Also from it"),
		"agent/skills/skip-me/SKILL.md": sk("skip-me", "Switched off by a pattern"),
		"agent/skills/keep-out/SKILL.md": sk("keep-out", "Switched off by an exact entry"),
		"agent/skills/stay/SKILL.md": sk("stay", "Stays"),
		"w/proj/.pi/settings.json": JSON.stringify({ skills: ["../../shared/s1/SKILL.md", "+nothing"] }),
		"w/shared/s1/SKILL.md": sk("s1", "A file named in project settings"),
	},
	expand: [],
	tools: [["read"]],
};
L.settings_untrusted = { ...L.settings, trusted: false };
L.ignore = {
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/.gitignore": "ignored-dir/\n*.tmp.md\n!keep.tmp.md\n# comment\n\n",
		"agent/skills/ok/SKILL.md": sk("ok", "Fine"),
		"agent/skills/ignored-dir/SKILL.md": sk("ignored-dir", "Hidden by .gitignore"),
		"agent/skills/skip.tmp.md": sk("skip-tmp", "Hidden by *.tmp.md"),
		"agent/skills/keep.tmp.md": sk("keep-tmp", "Brought back by the ! rule"),
		"agent/skills/sub/.ignore": "secret/\n",
		"agent/skills/sub/secret/SKILL.md": sk("secret", "Hidden by sub/.ignore"),
		"agent/skills/sub/fine/SKILL.md": sk("fine", "Fine, below sub"),
		"agent/skills/other/.fdignore": "x\n",
		"agent/skills/other/y/SKILL.md": sk("y", "The pattern x does not match y"),
	},
	expand: [],
	tools: [["read"]],
};
L.nested = {
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/top/SKILL.md": sk("top", "A skill root: not searched further"),
		"agent/skills/top/inner/SKILL.md": sk("inner", "Never found"),
		"agent/skills/deep/a/b/c/SKILL.md": sk("c", "Found four levels down"),
		"agent/skills/.hidden/SKILL.md": sk("hidden-dir", "A dot directory is skipped"),
		"agent/skills/node_modules/x/SKILL.md": sk("nm", "node_modules is skipped"),
		"agent/skills/mixed/readme.md": sk(null, "A markdown file below the root is not a skill here"),
		"agent/skills/mixed/real/SKILL.md": sk("real", "Next to it"),
		"agent/skills/tools.md": sk(null, "A markdown file at the root is"),
	},
	expand: [],
	tools: [["read"]],
};
L.yaml_errors = {
	// I8: every YAML error the yaml package reports (not only the nested mapping one) skips a SKILL.md with its message; a loose
	// .md with broken YAML is skipped silently; valid YAML the old subset got wrong loads
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/dupkey/SKILL.md": "---\nname: dupkey\ndescription: One\nname: again\n---\nBody\n",
		"agent/skills/unclosed/SKILL.md": '---\nname: unclosed\ndescription: "never closed\n---\nBody\n',
		"agent/skills/unclosed1/SKILL.md": "---\nname: unclosed1\ndescription: 'never closed\n---\nBody\n",
		"agent/skills/tabs/SKILL.md": "---\nname: tabs\ndescription: x\n\tindented: with a tab\n---\nBody\n",
		"agent/skills/flow/SKILL.md": "---\nname: flow\ndescription: [a, b\n---\nBody\n",
		"agent/skills/flowmap/SKILL.md": "---\nname: flowmap\ndescription: {a: b\n---\nBody\n",
		"agent/skills/at/SKILL.md": "---\nname: at\ndescription: @reserved\n---\nBody\n",
		"agent/skills/hdr/SKILL.md": "---\nname: hdr\ndescription: |0\n  text\n---\nBody\n",
		"agent/skills/escape/SKILL.md": '---\nname: escape\ndescription: "bad \\q escape"\n---\nBody\n',
		"agent/skills/alias/SKILL.md": "---\nname: alias\ndescription: *nothing\n---\nBody\n",
		"agent/skills/indent/SKILL.md": "---\nname: indent\ndescription: |\n   three\n  two\n---\nBody\n",
		"agent/skills/seqkey/SKILL.md": "---\n- not a mapping\nname: seqkey\n---\nBody\n",
		"agent/skills/multidoc/SKILL.md": "---\nname: multidoc\ndescription: One\n...\nname: two\n---\nBody\n",
		"agent/skills/loose-bad.md": "---\ndescription: dup\ndescription: dup\n---\nA loose file with broken YAML: skipped silently.\n",
		"agent/skills/anchor/SKILL.md": "---\nname: &n anchor\ndescription: *n\n---\nBody\n",
		"agent/skills/tagged/SKILL.md": "---\nname: !!str 123\ndescription: !!str true\n---\nBody\n",
		"agent/skills/keep/SKILL.md": "---\nname: keep\ndescription: |+\n  kept\n\n---\nBody\n",
		"agent/skills/foldedmore/SKILL.md": "---\nname: foldedmore\ndescription: >\n  a\n    more indented\n  b\n---\nBody\n",
		"agent/skills/explicit/SKILL.md": "---\n? name\n: explicit\n? description\n: An explicit key\n---\nBody\n",
		"agent/skills/flowtop/SKILL.md": "---\n{name: flowtop, description: A flow mapping}\n---\nBody\n",
		"agent/skills/comment-only/SKILL.md": "---\n# only a comment\n---\nBody\n",
		"agent/skills/nonascii/SKILL.md": "---\nname: nonascii\ndescription: \"caf\\u00e9 \\U0001F600 \\x41\"\n---\nBody\n",
		"agent/skills/nested-map/SKILL.md": "---\nname: nested-map\ndescription: Use when: this\n---\nBody\n",
	},
	expand: ["/skill:anchor", "/skill:tagged", "/skill:keep", "/skill:explicit"],
	tools: [["read"]],
};
L.order = {
	// I8: pi lists a directory in the order libuv's scandir gives it, sorted by bytes (not by case, not in creation order)
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/z.md": sk("zz", "Lower z"),
		"agent/skills/b.md": sk("bb", "Lower b"),
		"agent/skills/a.md": sk("aa", "Lower a"),
		"agent/skills/_u/SKILL.md": sk("under", "Underscore"),
		"agent/skills/Z/SKILL.md": sk("ZZ-up", "Upper Z directory"),
		"agent/skills/B.md": sk("bb-up", "Upper B"),
		"agent/skills/A.md": sk("aa-up", "Upper A"),
		"agent/skills/10/SKILL.md": sk("ten", "Ten"),
		"agent/skills/9/SKILL.md": sk("nine", "Nine"),
		"agent/skills/\u00e9/SKILL.md": sk("eacute", "Latin small e with acute"),
	},
	expand: [],
	tools: [["read"]],
};
L.ignore_case = {
	// I8: pi's ignore package matches without regard to case (its ignorecase option is on by default)
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/skills/.gitignore": "IGNORED-DIR/\n*.TMP.MD\n!KEEP.tmp.md\n",
		"agent/skills/ok/SKILL.md": sk("ok", "Fine"),
		"agent/skills/ignored-dir/SKILL.md": sk("ignored-dir", "Hidden by a pattern in capitals"),
		"agent/skills/skip.tmp.md": sk("skip-tmp", "Hidden by *.TMP.MD"),
		"agent/skills/keep.tmp.md": sk("keep-tmp", "Brought back by !KEEP.tmp.md"),
	},
	expand: [],
	tools: [["read"]],
};
L.rank = {
	// I8: resources are ranked before they are loaded (resourcePrecedenceRank): a project settings entry, the project's default
	// directories, a user settings entry, the user's default directories, packages. The first of two skills with one name wins.
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/settings.json": JSON.stringify({ skills: ["extra"] }),
		"agent/extra/dup/SKILL.md": sk("dup", "From a user settings entry (rank 2)"),
		"agent/skills/dup/SKILL.md": sk("dup", "From the user's directory (rank 3)"),
		"w/proj/.pi/skills/dup/SKILL.md": sk("dup", "From the project's directory (rank 1)"),
		"w/proj/.pi/settings.json": JSON.stringify({ skills: ["../more"] }),
		"w/more/dup2/SKILL.md": sk("dup2", "From a project settings entry (rank 0)"),
		"w/proj/.pi/skills/dup2/SKILL.md": sk("dup2", "From the project's directory (rank 1)"),
		"agent/extra/dup3/SKILL.md": sk("dup3", "From a user settings entry (rank 2)"),
		"agent/skills/dup3/SKILL.md": sk("dup3", "From the user's directory (rank 3)"),
		"agent/extra/solo/SKILL.md": sk("solo", "Only here"),
	},
	expand: [],
	tools: [["read"]],
};
L.tilde = {
	// I8: `~` in a path of settings.json or --skill is the home directory; the entry is trimmed (resolvePath, utils/paths.ts)
	cwd: "w/proj",
	trusted: true,
	skillPaths: ["~/cli-skills/c1", "  ~/cli-skills/c2/SKILL.md  ", "~/nothing-here"],
	files: {
		"agent/settings.json": JSON.stringify({ skills: ["~/shared/s1", "  ~/shared/s2/SKILL.md ", "~"] }),
		"home/shared/s1/SKILL.md": sk("s1", "From ~/shared/s1"),
		"home/shared/s2/SKILL.md": sk("s2", "From a trimmed entry"),
		"home/cli-skills/c1/SKILL.md": sk("c1", "From --skill ~/cli-skills/c1"),
		"home/cli-skills/c2/SKILL.md": sk("c2", "From a trimmed --skill"),
		"home/top/SKILL.md": sk("top", "Found because ~ itself is an entry"),
	},
	expand: [],
	tools: [["read"]],
};
L.tilde_overrides = {
	// P12-B7 (pi 1.2.0, #10765): `~` in a !pattern, +path or -path entry of settings.json is the home directory (package-manager.ts normalizePath)
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/settings.json": JSON.stringify({ skills: ["~/skills", "!~/skills/pdf", "-~/skills/zip", "+~/skills/zip"] }),
		"home/skills/pdf/SKILL.md": sk("pdf", "Switched off by !~/skills/pdf"),
		"home/skills/zip/SKILL.md": sk("zip", "Off by -~/skills/zip, which +~/skills/zip does not undo (the last one wins)"),
		"home/skills/keep/SKILL.md": sk("keep", "Stays"),
	},
	expand: [],
	tools: [["read"]],
};
L.globcase = {
	// I8: settings.json patterns are minimatch's: the case of letters counts (pi's ignore files do not care, see ignore_case)
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/settings.json": JSON.stringify({ skills: ["!Skip-*", "!skip-lower-*", "-Exact", "-exact-lower"] }),
		"agent/skills/Skip-Upper/SKILL.md": sk("Skip-Upper", "Matched by !Skip-*"),
		"agent/skills/skip-upper2/SKILL.md": sk("skip-upper2", "Not matched by !Skip-*: case counts"),
		"agent/skills/skip-lower-1/SKILL.md": sk("skip-lower-1", "Matched by !skip-lower-*"),
		"agent/skills/Skip-lower-2/SKILL.md": sk("skip-lower-2", "Not matched by !skip-lower-*"),
		"agent/skills/Exact/SKILL.md": sk("exact-up", "Removed by -Exact"),
		"agent/skills/exact/SKILL.md": sk("exact-lo", "Not removed by -Exact"),
		"agent/skills/exact-lower/SKILL.md": sk("exact-lower", "Removed by -exact-lower"),
		"agent/skills/Exact-Lower/SKILL.md": sk("exact-lower-up", "Not removed by -exact-lower"),
	},
	expand: [],
	tools: [["read"]],
};
L.packages = {
	// I8/J5: local packages of settings.json: a manifest (package.json "pi"), the conventional directories, an object entry that
	// filters, an entry that is not a local path (npm:, git:) is not fetched
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/settings.json": JSON.stringify({
			packages: ["../pk/manifest", "../pk/conv", { source: "../pk/filter", skills: ["keep-*", "!keep-no"] }, { source: "../pk/off", skills: [] }, "npm:some-package", "git:github.com/x/y"],
		}),
		"pk/manifest/package.json": JSON.stringify({ name: "m", pi: { skills: ["./custom", "extra/*.md", "!extra/skip.md"], prompts: ["./pr"] } }),
		"pk/manifest/custom/c1/SKILL.md": sk("c1", "A manifest entry"),
		"pk/manifest/extra/e1.md": sk("e1", "Matched by a manifest glob"),
		"pk/manifest/extra/skip.md": sk("skipped", "Switched off by the manifest"),
		"pk/manifest/skills/unlisted/SKILL.md": sk("unlisted", "Not in the manifest: not found"),
		"pk/conv/skills/v1/SKILL.md": sk("v1", "From a conventional directory"),
		"pk/conv/skills/v2/SKILL.md": sk("v2", "Another"),
		"pk/filter/skills/keep-a/SKILL.md": sk("keep-a", "Kept by the filter"),
		"pk/filter/skills/keep-no/SKILL.md": sk("keep-no", "Dropped by the filter"),
		"pk/filter/skills/other/SKILL.md": sk("other", "Not matched by the filter"),
		"pk/off/skills/off1/SKILL.md": sk("off1", "An empty list switches the type off"),
		"agent/skills/own/SKILL.md": sk("own", "Beats the packages"),
		"pk/conv/skills/own/SKILL.md": sk("own", "Same name, from a package"),
		"w/proj/.pi/settings.json": JSON.stringify({ packages: ["../../pk/projpkg"] }),
		"pk/projpkg/skills/pp/SKILL.md": sk("pp", "From a project package"),
	},
	expand: [],
	tools: [["read"]],
};
L.cli = {
	cwd: "w/proj",
	trusted: true,
	skillPaths: ["extra/one.md", "extra/dir", "extra/none.md", "extra/x.txt", "../abs/two.md", "@ROOT@/cli/w/abs-skill.md"],
	files: {
		"agent/skills/base/SKILL.md": sk("base", "Discovered"),
		"w/proj/extra/one.md": sk("one", "A file given with --skill"),
		"w/proj/extra/dir/d1/SKILL.md": sk("d1", "In a directory given with --skill"),
		"w/proj/extra/x.txt": "not markdown\n",
		"w/abs/two.md": sk("two", "Outside the project"),
		"w/abs-skill.md": sk("abs-skill", "An absolute host path"),
	},
	expand: ["/skill:one"],
	tools: [["read"]],
};
L.noskills = { ...L.cli, noSkills: true, skillPaths: ["extra/one.md"] };

function norm(root, layoutDir, s) {
	// ~/.agents/skills is <agent>/agents-skills in amiga-pi
	const home = join(layoutDir, "home");
	return s
		.split(join(home, ".agents", "skills")).join(join(layoutDir, "agent", "agents-skills"))
		.split(join(home, ".agents")).join(join(layoutDir, "agent"))
		.split(root).join("@ROOT@");
}

// interactive-mode.ts:676-708 getAutocompleteSourceTag / prefixAutocompleteDescription, 1456-1475 getDisplaySourceInfo
function tagOf(si) {
	const scopePrefix = si.scope === "user" ? "u" : si.scope === "project" ? "p" : "t";
	return scopePrefix;
}
function labelOf(si) {
	const source = si?.source ?? "local";
	const scope = si?.scope ?? "project";
	let label, scopeLabel;
	if (source === "local") {
		if (scope === "user") label = "user";
		else if (scope === "project") label = "project";
		else if (scope === "temporary") (label = "path"), (scopeLabel = "temp");
		else label = "path";
	} else if (source === "cli") (label = "path"), (scopeLabel = scope === "temporary" ? "temp" : undefined);
	else {
		label = source;
		scopeLabel = scope === "user" ? "user" : scope === "project" ? "project" : scope === "temporary" ? "temp" : undefined;
	}
	return scopeLabel ? `${label} (${scopeLabel})` : label;
}

const root = realpathSync(mkdtempSync(join(tmpdir(), "pi_skills_")));
mkdirSync(outDir, { recursive: true });
const layoutsOut = {};
for (const [name, lay] of Object.entries(L)) {
	const dir = join(root, name);
	const put = (rel, text) => {
		const p = join(dir, rel);
		mkdirSync(dirname(p), { recursive: true });
		writeFileSync(p, text);
	};
	for (const [rel, text] of Object.entries(lay.files)) put(rel, text);
	const cwd = join(dir, lay.cwd);
	const agentDir = join(dir, "agent");
	mkdirSync(cwd, { recursive: true });
	mkdirSync(agentDir, { recursive: true });
	// pi's ~/.agents/skills: the layout's "agent/agents-skills" goes to <home>/.agents/skills
	mkdirSync(join(dir, "home", ".agents"), { recursive: true });
	for (const [rel, text] of Object.entries(lay.files))
		if (rel.startsWith("agent/agents-skills/")) put(join("home", ".agents", "skills", rel.slice("agent/agents-skills/".length)), text);
	process.env.HOME = join(dir, "home");
	const settingsManager = pi.SettingsManager.create(cwd, agentDir, { projectTrusted: lay.trusted });
	const loader = new pi.DefaultResourceLoader({
		cwd,
		agentDir,
		settingsManager,
		noExtensions: true,
		noPromptTemplates: true,
		noThemes: true,
		noSkills: !!lay.noSkills,
		additionalSkillPaths: (lay.skillPaths ?? []).map((p) => piResolvePath(p.split("@ROOT@").join(root), cwd)), // main.ts resolveCliPaths
	});
	await loader.reload();
	const { skills, diagnostics } = loader.getSkills();
	const n = (s) => (s === undefined ? s : norm(root, dir, s));
	const out = {
		skills: skills.map((s) => ({
			name: s.name,
			description: s.description,
			filePath: n(s.filePath),
			baseDir: n(s.baseDir),
			disableModelInvocation: s.disableModelInvocation,
			label: labelOf(s.sourceInfo),
			command: `[${tagOf(s.sourceInfo)}] ${s.description}`,
		})),
		diagnostics: diagnostics.map((d) => ({
			type: d.type,
			message: n(d.message),
			path: n(d.path),
			...(d.collision ? { collision: { name: d.collision.name, winnerPath: n(d.collision.winnerPath), loserPath: n(d.collision.loserPath) } } : {}),
		})),
		enableSkillCommands: settingsManager.getEnableSkillCommands(),
		// rpc-mode.ts:701-708 get_commands for the skills (the same loop over the loader's skills)
		rpcCommands: n(JSON.stringify(skills.map((skill) => ({ name: `skill:${skill.name}`, description: skill.description, source: "skill", sourceInfo: skill.sourceInfo })))),
		sections: {},
		expand: [],
	};
	for (const tools of lay.tools ?? []) {
		const sections = buildSystemPromptSections({
			cwd,
			contextFiles: [],
			selectedTools: tools,
			hiddenTools: [],
			toolSnippets: Object.fromEntries(tools.map((t) => [t, `${t} snippet`])),
			toolGuidelines: {},
			promptGuidelines: [],
			sections: {},
			skills,
		});
		out.sections[tools.join("+")] = n(sections.skills) ?? null;
	}
	const fake = { resourceLoader: loader, _extensionRunner: { emitError() {} } };
	for (const input of lay.expand ?? []) out.expand.push({ input, output: n(AgentSession.prototype._expandSkillCommand.call(fake, input)) });
	writeFileSync(join(outDir, `${name}.json`), JSON.stringify(out, null, 1) + "\n");
	layoutsOut[name] = {
		cwd: lay.cwd,
		trusted: lay.trusted,
		noSkills: !!lay.noSkills,
		skillPaths: lay.skillPaths ?? [],
		expand: lay.expand ?? [],
		tools: lay.tools ?? [],
		files: lay.files,
	};
}
writeFileSync(join(outDir, "layouts.json"), JSON.stringify(layoutsOut, null, 1) + "\n");
rmSync(root, { recursive: true, force: true });
console.log("wrote", outDir, Object.keys(L).length, "layouts, pi", version);
