// gen_prompts.mjs -- the expected outputs of the prompt template discovery tests (tests/test_pi_templates.c, plan phase J5), produced
// by pi 1.2.0's own code: the pinned install vendor/pi-1.2.0 (tools/pi_oracle/pin.mjs), never the pi
// on the PATH. What runs is pi's DefaultResourceLoader (core/resource-loader.ts, core/package-manager.ts: the places templates are
// found in, settings.json "prompts" and "packages", the ignore files), loadPromptTemplates (core/prompt-templates.ts: the files, the
// frontmatter, the diagnostics) and expandPromptTemplate.
//
//   node tools/pi_oracle/gen_prompts.mjs          writes tests/pi/prompts/layouts.json and <layout>.json
//
// The trees of every layout are described once, here, and written to layouts.json; the test makes the same trees in its own temp
// directory and compares what pi_tmpl_load gives with <layout>.json. Paths in the output have the layout's directory replaced by
// @ROOT@. Every layout has its own temporary directory (nothing fixed), and HOME is the layout's "home" directory.
import { mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { caDir } from "./pin.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const outDir = resolve(here, "..", "..", "tests", "pi", "prompts");
process.env.PI_OFFLINE = "1"; // a package source npm:/git: is not fetched: pi skips it
const pkg = caDir;
const pi = await import(pathToFileURL(join(pkg, "dist", "index.js")).href);
const { expandPromptTemplate } = await import(pathToFileURL(join(pkg, "dist", "core", "prompt-templates.js")).href);
const { resolvePath: piResolvePath } = await import(pathToFileURL(join(pkg, "dist", "utils", "paths.js")).href);
const version = JSON.parse(readFileSync(join(pkg, "package.json"), "utf-8")).version;
process.removeAllListeners("warning");
process.on("warning", () => {});

const tp = (desc, body, extra = "") => `---\n${desc === null ? "" : `description: ${desc}\n`}${extra}---\n${body}\n`;

// layout: files {path relative to the layout dir: text}, cwd, trusted, noTemplates, promptPaths (as typed after --prompt-template),
// expand (inputs for expandPromptTemplate)
const L = {};
L.basic = {
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/prompts/review.md": tp("Review code", "User review of $1", 'argument-hint: "<path> [focus]"\n'),
		"agent/prompts/sum.md": "---\r\ndescription: 'Sum it'\r\n---\r\n\r\n  body $@  \r\n\r\n",
		"agent/prompts/long.md": "\n   \nThis first line is much longer than sixty characters, so it is cut and dots follow\nsecond\n",
		"agent/prompts/.hidden.md": "hidden\n",
		"agent/prompts/notes.txt": "not a template\n",
		"agent/prompts/sub/deep.md": "not scanned: the directory is not recursive\n",
		"agent/prompts/node_modules/x.md": "in node_modules\n",
		"w/proj/.pi/prompts/review.md": tp("Project review", "Project review of $ARGUMENTS"),
		"w/proj/.pi/prompts/fix.md": "Fix $1\n",
	},
	expand: ["/review a b", "/fix x", "/sum 1 2", "/deep", "/nothing"],
};
L.untrusted = { ...L.basic, trusted: false, expand: ["/review a b", "/fix x"] };
L.order = {
	// readdirSync order in pi is libuv's scandir sorted by bytes: not by case, not in creation order
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/prompts/z.md": "z\n",
		"agent/prompts/b.md": "b\n",
		"agent/prompts/a.md": "a\n",
		"agent/prompts/B.md": "B\n",
		"agent/prompts/A.md": "A\n",
		"agent/prompts/_u.md": "under\n",
		"agent/prompts/10.md": "ten\n",
		"agent/prompts/9.md": "nine\n",
		"agent/prompts/\u00e9.md": "eacute\n",
	},
	expand: [],
};
L.ignore = {
	// .gitignore, .ignore and .fdignore of the prompts directory (collectAutoPromptEntries uses the ignore package); case does not count
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/prompts/.gitignore": "ignored.md\n*.tmp.md\n!keep.tmp.md\n# comment\n\nSUB/\nBIG.md\n",
		"agent/prompts/.ignore": "from-ignore.md\n",
		"agent/prompts/.fdignore": "from-fdignore.md\n/anchored.md\n",
		"agent/prompts/ok.md": "ok\n",
		"agent/prompts/ignored.md": "hidden by .gitignore\n",
		"agent/prompts/skip.tmp.md": "hidden by *.tmp.md\n",
		"agent/prompts/keep.tmp.md": "brought back by !keep.tmp.md\n",
		"agent/prompts/from-ignore.md": "hidden by .ignore\n",
		"agent/prompts/from-fdignore.md": "hidden by .fdignore\n",
		"agent/prompts/anchored.md": "hidden by /anchored.md\n",
		"agent/prompts/big.md": "hidden by BIG.md (case does not count)\n",
		"agent/prompts/sub.md": "a file called sub.md: SUB/ is for directories\n",
		"w/proj/.pi/prompts/.gitignore": "private-*\n",
		"w/proj/.pi/prompts/private-x.md": "hidden in the project\n",
		"w/proj/.pi/prompts/public-x.md": "project\n",
	},
	expand: [],
};
L.settings = {
	// settings.json "prompts": a directory is searched below (recursively, with the ignore files), a file is taken, patterns switch
	// templates off or on; the project's entries are relative to <cwd>/.pi, the user's to the agent directory
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/settings.json": JSON.stringify({ prompts: ["extra", "single.md", "!skip-*", "-agent/prompts/off.md", "../shared/s.md"] }),
		"agent/extra/e1.md": "e1\n",
		"agent/extra/deep/e2.md": "e2 below extra: the search is recursive\n",
		"agent/extra/skip-me.md": "switched off by !skip-*\n",
		"agent/extra/.hidden.md": "hidden\n",
		"agent/extra/node_modules/n.md": "node_modules\n",
		"agent/extra/.gitignore": "gone.md\n",
		"agent/extra/gone.md": "ignored\n",
		"agent/single.md": "single\n",
		"agent/shared-not.md": "never named\n",
		"shared/s.md": "named with ..\n",
		"agent/prompts/off.md": "switched off by an exact entry -agent/prompts/off.md that does not match: -x matches paths relative to the base\n",
		"agent/prompts/stay.md": "stays\n",
		"w/proj/.pi/settings.json": JSON.stringify({ prompts: ["../../local-prompts", "!nope-*", "+never-there.md"] }),
		"w/local-prompts/lp1.md": "lp1\n",
		"w/local-prompts/nope-1.md": "nope\n",
	},
	expand: ["/e1", "/e2", "/lp1"],
};
L.settings_untrusted = { ...L.settings, trusted: false, expand: [] };
L.tilde = {
	// `~` in settings.json and --prompt-template is the home directory; entries are trimmed
	cwd: "w/proj",
	trusted: true,
	promptPaths: ["~/cli-prompts", "  ~/cli-one.md  ", "~/none.md"],
	files: {
		"agent/settings.json": JSON.stringify({ prompts: ["~/shared-prompts", "  ~/shared-one.md ", "~/shared-prompts/sub/../other"] }),
		"home/shared-prompts/p1.md": "p1\n",
		"home/shared-prompts/other/p2.md": "p2\n",
		"home/shared-one.md": "one\n",
		"home/cli-prompts/c1.md": "c1\n",
		"home/cli-one.md": "cli one\n",
	},
	expand: [],
};
L.globcase = {
	// settings.json patterns are minimatch's: the case of letters counts
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/settings.json": JSON.stringify({ prompts: ["!Skip-*", "!skip-lower-*"] }),
		"agent/prompts/Skip-Upper.md": "matched by !Skip-*\n",
		"agent/prompts/skip-upper2.md": "not matched by !Skip-*: case counts\n",
		"agent/prompts/skip-lower-1.md": "matched by !skip-lower-*\n",
		"agent/prompts/Skip-lower-2.md": "not matched by !skip-lower-*\n",
	},
	expand: [],
};
L.yaml = {
	// a template whose frontmatter YAML is refused is skipped with a warning (the yaml package's message); valid but unusual YAML loads
	cwd: "w/proj",
	trusted: false,
	files: {
		"agent/prompts/dupkey.md": "---\ndescription: One\ndescription: Two\n---\nbody\n",
		"agent/prompts/unclosed.md": '---\ndescription: "never closed\n---\nbody\n',
		"agent/prompts/colon.md": "---\ndescription: Use when: this\n---\nbody\n",
		"agent/prompts/tabs.md": "---\ndescription: x\n\tindented: tab\n---\nbody\n",
		"agent/prompts/flow.md": "---\ndescription: [a, b\n---\nbody\n",
		"agent/prompts/folded.md": "---\ndescription: >-\n  folded\n  text\nargument-hint: |\n  <a>\n  <b>\n---\nbody\n",
		"agent/prompts/number.md": "---\ndescription: 123\nargument-hint: true\n---\nThe first line of the body\n",
		"agent/prompts/quoted.md": '---\ndescription: "Tab\\there \\u00e9"\nargument-hint: \'<it\'\'s>\'\n---\nbody\n',
		"agent/prompts/emptyfm.md": "---\n---\nthe body only\n",
		"agent/prompts/null.md": "---\ndescription:\nargument-hint: ~\n---\nnull values\n",
		"agent/prompts/explicit.md": "---\n? description\n: An explicit key\n---\nbody\n",
		"agent/prompts/bom.md": "\ufeff---\ndescription: After a BOM\n---\nbody\n",
		"agent/prompts/blockerr.md": "---\ndescription: |0\n  x\n---\nbody\n",
	},
	expand: ["/colon x", "/folded"],
};
L.collision = {
	// one name in several places: the first by precedence wins, the others are collisions with both paths
	cwd: "w/proj",
	trusted: true,
	promptPaths: ["cli/dup.md", "cli/only-cli.md"],
	files: {
		"agent/settings.json": JSON.stringify({ prompts: ["entry"] }),
		"agent/entry/dup.md": "from a user settings entry (rank 2)\n",
		"agent/prompts/dup.md": "from the user's directory (rank 3)\n",
		"w/proj/.pi/prompts/dup.md": "from the project's directory (rank 1)\n",
		"w/proj/.pi/settings.json": JSON.stringify({ prompts: ["../../more"] }),
		"w/more/dup2.md": "from a project settings entry (rank 0)\n",
		"w/proj/.pi/prompts/dup2.md": "from the project's directory (rank 1)\n",
		"agent/prompts/dup3.md": "user directory\n",
		"agent/entry/dup3.md": "user entry (wins over the directory)\n",
		"w/proj/cli/dup.md": "from --prompt-template (last)\n",
		"w/proj/cli/only-cli.md": "only from --prompt-template\n",
	},
	expand: ["/dup", "/dup2", "/dup3", "/only-cli"],
};
L.packages = {
	// local packages of settings.json: a manifest, the conventional directory, an object entry that filters; npm: and git: are skipped
	cwd: "w/proj",
	trusted: true,
	files: {
		"agent/settings.json": JSON.stringify({
			packages: ["../pk/manifest", "../pk/conv", { source: "../pk/filter", prompts: ["keep-*", "!keep-no*"] }, { source: "../pk/off", prompts: [] }, "npm:some-package", "git:github.com/x/y"],
		}),
		"pk/manifest/package.json": JSON.stringify({ name: "m", pi: { prompts: ["./pr", "more/*.md", "!more/skip.md"] } }),
		"pk/manifest/pr/m1.md": "m1\n",
		"pk/manifest/more/m2.md": "m2\n",
		"pk/manifest/more/skip.md": "switched off by the manifest\n",
		"pk/manifest/prompts/unlisted.md": "not in the manifest: not found\n",
		"pk/conv/prompts/v1.md": "v1\n",
		"pk/conv/prompts/deep/v2.md": "v2: a package's prompts are searched recursively\n",
		"pk/conv/prompts/.hidden.md": "hidden\n",
		"pk/filter/prompts/keep-a.md": "kept\n",
		"pk/filter/prompts/keep-no-1.md": "dropped\n",
		"pk/filter/prompts/other.md": "not matched\n",
		"pk/off/prompts/off1.md": "switched off by an empty list\n",
		"agent/prompts/own.md": "beats the package\n",
		"pk/conv/prompts/own.md": "same name, from a package\n",
		"w/proj/.pi/settings.json": JSON.stringify({ packages: ["../../../pk/projpkg"] }),
		"pk/projpkg/prompts/pp.md": "from a project package\n",
	},
	expand: ["/v1", "/own"],
};
L.cli = {
	// --prompt-template: a file, a directory (read whole, hidden files too, not recursive), a path that is not there, a file that is not
	// markdown, a path twice; the defaults come first; -np leaves them out
	cwd: "w/proj",
	trusted: true,
	promptPaths: ["extra/one.md", "extra/dir", "extra/none.md", "extra/x.txt", "../abs/two.md", "extra/one.md", "@ROOT@/cli/w/abs-one.md"],
	files: {
		"agent/prompts/base.md": "base\n",
		"w/proj/extra/one.md": "one\n",
		"w/proj/extra/dir/d1.md": "d1\n",
		"w/proj/extra/dir/.d2.md": "hidden but read\n",
		"w/proj/extra/dir/sub/d3.md": "not read: not recursive\n",
		"w/proj/extra/dir/ignored.md": "ignore files are not read for a directory given by hand\n",
		"w/proj/extra/dir/.gitignore": "ignored.md\n",
		"w/proj/extra/x.txt": "not markdown\n",
		"w/abs/two.md": "outside the project\n",
		"w/abs-one.md": "an absolute host path\n",
	},
	expand: ["/one", "/d1", "/abs-one"],
};
L.nodefaults = { ...L.cli, noTemplates: true, promptPaths: ["extra/one.md", "extra/dir"] };

function norm(root, s) {
	return s === undefined ? s : s.split(root).join("@ROOT@");
}

// interactive-mode.ts:676-708 getAutocompleteSourceTag
function tagOf(si) {
	const scopePrefix = si.scope === "user" ? "u" : si.scope === "project" ? "p" : "t";
	const source = si.source.trim();
	if (source === "auto" || source === "local" || source === "cli") return scopePrefix;
	return scopePrefix;
}

const root = realpathSync(mkdtempSync(join(tmpdir(), "pi_prompts_")));
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
	mkdirSync(join(dir, "home"), { recursive: true });
	process.env.HOME = join(dir, "home");
	const settingsManager = pi.SettingsManager.create(cwd, agentDir, { projectTrusted: lay.trusted });
	const loader = new pi.DefaultResourceLoader({
		cwd,
		agentDir,
		settingsManager,
		noExtensions: true,
		noSkills: true,
		noThemes: true,
		noPromptTemplates: !!lay.noTemplates,
		additionalPromptTemplatePaths: (lay.promptPaths ?? []).map((p) => piResolvePath(p.split("@ROOT@").join(root), cwd)), // main.ts resolveCliPaths
	});
	await loader.reload();
	const { prompts, diagnostics } = loader.getPrompts();
	const n = (s) => norm(root, s);
	const out = {
		prompts: prompts.map((t) => ({
			name: t.name,
			description: t.description,
			argumentHint: t.argumentHint ?? null,
			content: t.content,
			filePath: n(t.filePath),
			sourceInfo: JSON.parse(n(JSON.stringify(t.sourceInfo))),
			command: `${t.argumentHint ? t.argumentHint + " \u2014 " : ""}[${tagOf(t.sourceInfo)}] ${t.description}`,
		})),
		diagnostics: diagnostics.map((d) => ({
			type: d.type,
			message: n(d.message),
			path: n(d.path),
			...(d.collision ? { collision: { name: d.collision.name, winnerPath: n(d.collision.winnerPath), loserPath: n(d.collision.loserPath) } } : {}),
		})),
		// rpc-mode.ts:690-696 get_commands for the prompt templates
		rpcCommands: n(JSON.stringify(prompts.map((t) => ({ name: t.name, description: t.description, source: "prompt", sourceInfo: t.sourceInfo })))),
		expand: (lay.expand ?? []).map((input) => ({ input, output: expandPromptTemplate(input, prompts) })),
	};
	writeFileSync(join(outDir, `${name}.json`), JSON.stringify(out, null, 1) + "\n");
	layoutsOut[name] = {
		cwd: lay.cwd,
		trusted: lay.trusted,
		noTemplates: !!lay.noTemplates,
		promptPaths: lay.promptPaths ?? [],
		expand: lay.expand ?? [],
		files: lay.files,
	};
}
writeFileSync(join(outDir, "layouts.json"), JSON.stringify(layoutsOut, null, 1) + "\n");
rmSync(root, { recursive: true, force: true });
console.log("wrote", outDir, Object.keys(L).length, "layouts, pi", version);
