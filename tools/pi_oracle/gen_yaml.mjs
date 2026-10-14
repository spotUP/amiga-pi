// gen_yaml.mjs -- the expected results of tests/test_pi_yaml.c, produced by pi 1.2.0's own utils/frontmatter.ts parseFrontmatter (which
// calls the yaml package's `parse`): the pinned install vendor/pi-1.2.0 (tools/pi_oracle/pin.mjs),
// never the pi on the PATH.
//
//   node tools/pi_oracle/gen_yaml.mjs        writes tests/pi/yaml/cases.json
//
// Each case is a whole file's content (the frontmatter between "---" lines and a body) for amiga-pi's pi_fm_parse. The expected result
// is pi's error message (the YAML is refused) or what the four keys the skills and prompt templates read are (absent, a string,
// true, or something else) and the body. The cases are curated YAML (every error the yaml package has a message for that frontmatter
// can reach) and a seeded set of random edits of them and random documents, 20000 times more of which agreed with the package
// during development (the generator is the same code).
import { readFileSync, realpathSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { caDir } from "./pin.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const outDir = resolve(here, "..", "..", "tests", "pi", "yaml");
const pkg = caDir;
const pi = await import(pathToFileURL(join(pkg, "dist", "index.js")).href);
const version = JSON.parse(readFileSync(join(pkg, "package.json"), "utf-8")).version;
process.removeAllListeners("warning");
process.on("warning", () => {});

const seeds = [
 "name: a\nname: b", "description: \"abc", "description: 'abc", "description:\ta", "\tname: a", "name: a\n\tb: c",
 "description: [a, b", "description: {a: b", "description: a: b", "description: a\n  b: c", "name: a\n b: c", "name: a\n  - b",
 "- a\nb: c", "a: b\n- c", "name: *x", "name: &x a\ndescription: *x", "name: @abc", "name: `abc", "name: %abc", "name: !!str 12",
 "name: !!int abc", "name: |\n  a\n b", "name: >\nabc", "? a\n: b", "name: a #c\n#x\ndescription: b", "name: a\n---\nb: c",
 "name: \"a\\qb\"", "name: \"a\\x4\"", "name: [a,, b]", "name: {a, b}", "name: {a: 1, a: 2}", "name: a\n description: b", "key",
 "- a\n- b", "[a, b]", "{a: b}", "a: b\n  c\nd: e", "name:  a\n    b\n  c: d", "name: ]", "name: }", "name: ,", "name: a,b",
 "name: -", "name: - a", "name: ? a", "name: | x", "name: >-2\n  a", "name: |0\n a",
 "name: my-skill\ndescription: Use when: this", "name: my-skill\ndescription: A skill that does things\ndisable-model-invocation: true",
 "description: >\n  folded text\n  more\n\n  para\nname: x", "description: |\n  literal\n   indented\n  back\n", "description: |+\n  keep\n\n\nname: y",
 "description: |-\n  strip\n\n", "description: \"multi\n  line \\\n  continued\"", "description: 'it''s'", "description: \"tab\\there \\u00e9 \\x41\"",
 "name: x\ndescription: y # comment\ndisable-model-invocation: True", "disable-model-invocation: yes", "disable-model-invocation: TRUE", "description: 123",
 "description: ~", "description:", "description: null", "description: [a, b]\nname: z", "description: {a: b}", "description: !!str true", "description: !custom x",
 "&a name: x", "name: &a x\ndescription: *a", "name: !!binary aGk=", "description: !!timestamp 2001-01-01", "name: x\n# only comment",
 "# comment\nname: x", "---\nname: x", "name: x\n...\nname: y", "%YAML 1.2\n---\nname: x", "%TAG !e! tag:e.com,2000:\n---\nname: !e!foo x", "name: !e!foo x",
 "? name\n: x\n? description\n: y", "name: [a,\n  b]\ndescription: c", "name: {a: [1, 2], b: {c: d}}", "a:\n  b:\n    c: d\n  e: f\ndescription: g",
 "a:\n- 1\n- 2\ndescription: g", "a:\n  - 1\n  - 2\ndescription: g", "description: \"unterminated\nname: x", "description: 'x\ny'\nname: z",
 "name: \"\\u00zz\"", "name: \"\\U00110000\"", "description: x\n    deep\n  mid", "k: v\n k2: v2", "k:\n    v\n  w", "- - a\n  - b", "a: !!seq [1]", "a: !!map {b: 1}",
 "a: &x [1, 2]\nb: *x", "a: *a", "&a [*a]", "name: x: y", "name: \"x\": y", "[a]: b", "{a}: b", "? [a, b]\n: c", "name: >2\n   x", "name: |2\n x",
 "description: \"\\", "name: '", "name: \"", "name: [", "name: {", "name: [a: b]", "name: [a: b, c]", "name: {a: b, c}", "name: [? a: b]", "name: !", "name: !!",
 "name: &", "name: *", "name: &a", "name: ! a", "key: value\n\n\n\n", "key: value\n  \n", "\n\nname: x", "   \nname: x", "name: x   ", "name:    x   #y",
 "a: 0o17\nb: 0x1F\nc: 1e3\nd: .5\ne: -.inf\nf: .nan\ng: 0.\nname: n", "1: a\n1.0: b", "true: a\nTrue: b", "null: a\n~: b", "'a': 1\n\"a\": 2", "a: 1\n'a': 2",
 "name: a\r\nb: c", "description: é€😀 ok", "description: \"é\\é\"", "\"é\": 1\n\"é\": 2", "é: 1\nname: x", "name:\n  - a\n  - b\ndescription: z",
 "name: x\n  - y", "name:\n- a\n-  b\n", "name:\n  a: 1\n  b: 2\n description: q", "name: x\n  y: z",
];

let st = 20261013;
const rnd = (n) => {
	st ^= st << 13;
	st >>>= 0;
	st ^= st >>> 17;
	st ^= st << 5;
	st >>>= 0;
	return st % n;
};
const pick = (a) => a[rnd(a.length)];
const POOL = [":", ": ", "- ", "? ", "#", " #", '"', "'", "[", "]", "{", "}", ",", "|", ">", "&a ", "*a", "!", "!!str ", "\t", "  ", " ", "\n", "\n  ", "\n- ", "\\", "%", "@", "`", "---", "...", "é", "0", "1", "~", "true", "null", "|-", ">+", "|2"];
function mutate(s) {
	const k = 1 + rnd(3);
	for (let i = 0; i < k; i++) {
		const op = rnd(9);
		const p = rnd(s.length + 1);
		if (op === 0 && s.length) s = s.slice(0, p) + s.slice(p + 1);
		else if (op === 1) s = s.slice(0, p) + pick(POOL) + s.slice(p);
		else if (op === 2 && s.length) s = s.slice(0, p) + pick(POOL) + s.slice(p + 1);
		else if (op === 3) { const lines = s.split("\n"); const l = rnd(lines.length); lines.splice(l, 0, lines[l]); s = lines.join("\n"); }
		else if (op === 4) { const lines = s.split("\n"); const l = rnd(lines.length); lines.splice(l, 1); s = lines.join("\n"); }
		else if (op === 5) { const lines = s.split("\n"); const l = rnd(lines.length); lines[l] = " ".repeat(rnd(5)) + lines[l]; s = lines.join("\n"); }
		else if (op === 6) s = s + "\n" + pick(seeds);
		else if (op === 7) s = s.slice(0, p);
		else if (op === 8) { const t = pick(seeds); const q = rnd(t.length + 1); s = s.slice(0, p) + t.slice(q, q + 1 + rnd(12)) + s.slice(p); }
	}
	return s;
}
const WORDS = ["name", "description", "a", "b", "key", "x y", "disable-model-invocation", "argument-hint", "1", "true", "null", "~", "é", "foo-bar", "http://x", "a:b", "#c", "- d", "?q", "'q'", '"q"', "[", "]", "{", "}", "&a", "*a", "!t", "!!str", "|", ">", "%", "@", "`", ",", "...", "---", "yes", "0x1F", "1e3", ".inf", "a b c d e f g"];
const SEPS = ["", " ", "  ", "\t", " \t"];
function scalar() {
	switch (rnd(9)) {
		case 0: return pick(WORDS);
		case 1: return '"' + pick(WORDS) + (rnd(4) ? '"' : "");
		case 2: return "'" + pick(WORDS) + (rnd(4) ? "'" : "");
		case 3: return "[" + Array.from({ length: rnd(4) }, () => scalar()).join(pick([", ", ",", " , ", ""])) + (rnd(5) ? "]" : "");
		case 4: return "{" + Array.from({ length: rnd(4) }, () => pick(WORDS) + pick([": ", ":", " : ", ""]) + scalar()).join(", ") + (rnd(5) ? "}" : "");
		case 5: return pick(["|", ">", "|-", ">+", "|2", ">-1", "| #c", "|x"]);
		case 6: return pick(["&a ", "*a", "!!str ", "!x ", "&b !!int "]) + pick(WORDS);
		case 7: return pick(WORDS) + " " + pick(WORDS);
		default: return pick(WORDS);
	}
}
function line(ind) {
	const sp = " ".repeat(ind) + (rnd(12) === 0 ? "\t" : "");
	switch (rnd(8)) {
		case 0: return sp + "- " + scalar();
		case 1: return sp + pick(["- ", "? ", ": "]) + pick(WORDS) + pick(SEPS);
		case 2: return sp + "# " + pick(WORDS);
		case 3: return sp;
		case 4: return sp + scalar();
		default: return sp + pick(WORDS) + pick([":", ": ", " : ", ":  "]) + (rnd(5) ? scalar() : "") + (rnd(8) === 0 ? " # c" : "");
	}
}
const LONG = "Use this skill when the user asks for something long and winding that goes past the eighty column mark for sure and then some";
const W3 = ["name", "description", "x", "Use when: this", LONG, "é" + LONG.slice(0, 50), "a #b", "$HOME", "it's", 'say "hi"', "C:\\path", "tab\there", "{not flow", "[not seq", "100%", "@home", "a: b: c"];
function val3() {
	switch (rnd(10)) {
		case 0: return pick(["|", ">", "|-", ">-", "|+", ">+", "|1", ">2", "|-2"]) + pick(["", " # c", "  "]) + "\n" + Array.from({ length: rnd(5) }, () => " ".repeat(rnd(5)) + pick(W3)).join("\n");
		case 1: return '"' + pick(W3).replace(/"/g, "") + pick(["", "\\n", "\\t", "\\u00e9", "\\q", "\\x4", "\\", "\n  cont"]) + pick(['"', '"', ""]);
		case 2: return "'" + pick(W3).replace(/'/g, "") + pick(["", "''", "\n  cont"]) + pick(["'", "'", ""]);
		case 3: return "[" + pick(W3) + ", " + pick(W3) + pick(["]", "", "\n]", "\n  ]"]);
		case 4: return pick(["&a ", "*a", "!!str ", "!!int ", "!!float ", "!!bool ", "!!null ", "!!timestamp ", "!!binary ", "!!set ", "!!map ", "!x "]) + pick(W3);
		case 5: return pick(W3) + "\n  " + pick(W3);
		default: return pick(W3);
	}
}
const wrap = (y) => "---\n" + y + "\n---\nBody of the file.\n\nSecond paragraph.\n";
const cases = [];
for (const s of seeds) cases.push(wrap(s));
for (let i = 0; i < 350; i++) cases.push(wrap(mutate(pick(seeds)).toWellFormed()));
for (let i = 0; i < 400; i++) {
	const n = 1 + rnd(7);
	let ind = 0;
	const L = [];
	for (let j = 0; j < n; j++) { if (rnd(3) === 0) ind = rnd(5) * (rnd(2) ? 1 : 2); L.push(line(ind)); }
	cases.push(wrap(L.join("\n").toWellFormed()));
}
const HEAD = ["", "%YAML 1.2\n---\n", "%TAG !e! tag:e.com,2000:\n", "%FOO\n", "# c\n", "---\n", "\n", "  \n"];
for (let i = 0; i < 300; i++) {
	const n = 1 + rnd(5);
	const L = [];
	for (let j = 0; j < n; j++) L.push(" ".repeat(rnd(3) === 0 ? rnd(4) : 0) + pick(["name", "description", "disable-model-invocation", "argument-hint", "k", "x y"]) + pick([": ", ":", ":  ", " : "]) + val3());
	cases.push(wrap((pick(HEAD) + L.join("\n") + pick(["", "\n", "\n...", "\n...\nname: z", "\n# end"])).toWellFormed()));
}
// whole-file cases: no frontmatter, a BOM, CR and CRLF, an empty block, ----, a body trimmed with Unicode spaces
for (const c of [
	"No frontmatter at all.\n", "", "---", "---\n", "---\n---\nbody\n", "---\n\n---\nbody\n", "----\nname: x\n---\nb", "---x\nname: y\n---\nb",
	"\ufeff---\nname: a\ndescription: d\n---\nbody", "---\r\nname: a\r\ndescription: crlf\r\n---\r\nbody\r\n", "---\rname: a\rdescription: cr\r---\rbody",
	"---\nname: a\ndescription: d\n---\n\u00a0\u2003 body with unicode spaces \u3000\n\n", "---\nname: a\n--- trailing\nbody", "---\nname: a\n----\nbody",
	"---\nname: a\nno closing line\n", "---\n---", "---\n---\n", "--- \nname: a\n---\nb",
]) cases.push(c);
const KEYS = ["name", "description", "disable-model-invocation", "argument-hint"];
const out = [];
for (const c of cases) {
	let rec;
	try {
		const { frontmatter, body } = pi.parseFrontmatter(c);
		rec = { c, k: KEYS.map((k) => { const v = frontmatter[k]; return v === undefined ? "n" : typeof v === "string" ? "s:" + v : v === true ? "t" : "o"; }), b: body };
	} catch (e) {
		rec = { c, e: e.message };
	}
	out.push(rec);
}
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "cases.json"), JSON.stringify(out, null, 0).replace(/\},\{/g, "},\n{") + "\n");
console.log("wrote", outDir, out.length, "cases,", out.filter((r) => r.e).length, "errors, pi", version);
