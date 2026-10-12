// Golden for tests/test_pi_templates.c: pi 1.1.0's own parseCommandArgs, substituteArgs and expandPromptTemplate
// (core/prompt-templates.ts, run from the pinned install's dist/), every case written as pi answers it.
//   node tools/pi_oracle/gen_templates.mjs      (PI_NPM=<pi install> to name another; default vendor/pi-1.1.0)
// Writes tests/pi/templates/cases.json: {"subst": [{c, a, args, out}], "expand": {templates, cases: [{text, out}]}}
import { writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const npm = process.env.PI_NPM || join(root, "vendor/pi-1.1.0");
const mod = await import(pathToFileURL(join(npm, "node_modules/@earendil-works/pi-coding-agent/dist/core/prompt-templates.js")).href);
const { parseCommandArgs, substituteArgs, expandPromptTemplate } = mod;
// [template content, the argument string typed after the name]
const subst = [
  ["$1", "a b c"], ["$2", "a b c"], ["$3 $2 $1", "a b c"], ["$4", "a b c"], ["$0", "a b c"], ["$10", "1 2 3 4 5 6 7 8 9 10 11"],
  ["$1", ""], ["x$1y", ""], ["$@", "a b c"], ["$@", ""], ["$ARGUMENTS", "a b c"], ["$ARGUMENTS", ""], ["$ARGUMENTSX", "a"],
  ["${1:-default}", "a"], ["${1:-default}", ""], ["${2:-default}", "a"], ["${0:-d}", "a"], ["${1:-}", ""], ["${1:-a b c}", ""],
  ["${@:-none}", ""], ["${@:-none}", "x y"], ["${ARGUMENTS:-none}", ""], ["${ARGUMENTS:-none}", "x"], ["${1:-no}}", ""],
  ["${@:2}", "a b c d"], ["${@:1}", "a b c d"], ["${@:0}", "a b c d"], ["${@:5}", "a b c d"], ["${@:4}", "a b c d"],
  ["${@:2:2}", "a b c d"], ["${@:2:0}", "a b c d"], ["${@:1:1}", "a b c d"], ["${@:3:10}", "a b c d"], ["${@:0:2}", "a b c d"],
  ["${@:2:}", "a b c d"], ["${@:}", "a b c d"], ["${@}", "a b"], ["${1}", "a b"], ["${1:-x", ""], ["${1:x}", "a"],
  ["$", "a"], ["$$1", "a"], ["$a", "a"], ["costs $5 or $ARGUMENTS.", "7"],
  ["$1", "'two words' 'it''s'"], ["$1|$2", "\"two words\" \"three  spaces here\""], ["$1", "a\"b c\"d"], ["$1", "\"unterminated quote"],
  ["$1|$2", "'a \"b\" c' d"], ["$@", "'x y'  z"], ["$1", "\"\" a"], ["$1|$2", "a '' b"], ["$ARGUMENTS", "  lead  and   trail  "],
  ["$1|$2|$3", "a\tb\nc"], ["$1", "café über"], ["$1|$2", "a b c"], ["$1|$2", "a　b"],
  ["$1 $1 $1", "r"], ["$1", "$2 $@"], ["$@ and ${1:-$2}", "$1"], ["${1:-$@}", ""], ["${1:-\\$1}", ""],
  ["line1\n$1\nline3 ${@:2}\n", "a b c"], ["no placeholders", "a b"], ["", "a b"], ["$1$2$3", "a b c"], ["$@$@", "a b"],
  ["${@:1:2} / ${@:3}", "a b c d e"], ["${3:-x}${2:-y}${1:-z}", "a"], ["${10:-ten}", "a"],
  ["$99999999999", "a"], ["${@:99999999999}", "a"], ["${@:1:99999999999}", "a b"],
];
const out = { subst: subst.map(([c, a]) => { const args = parseCommandArgs(a); return { c, a, args, out: substituteArgs(c, args) }; }) };
const templates = [
  { name: "review", content: "Review $1 focusing on ${2:-quality}. All: $@" },
  { name: "plain", content: "plain body\n" },
  { name: "ns:sub", content: "[$ARGUMENTS]" },
  { name: "dup", content: "first" },
  { name: "dup", content: "second" },
];
const texts = ["/review src", "/review", "/review src  tests\nmore", "/review   ", "/review\n", "/review\tsrc", "/plain", "/plain x y", "/ns:sub a b",
  "/dup", "/nosuch x", "/Review src", "review src", " /review src", "/", "//review", "/review src", "/review src \"a b\" 'c d'", "/plain\n\nx", "/review src\n\n"];
out.expand = { templates, cases: texts.map((t) => ({ text: t, out: expandPromptTemplate(t, templates) })) };
writeFileSync(join(root, "tests/pi/templates/cases.json"), JSON.stringify(out, null, 1) + "\n");
console.log("[OK] " + out.subst.length + " substitution cases, " + texts.length + " expand cases");
