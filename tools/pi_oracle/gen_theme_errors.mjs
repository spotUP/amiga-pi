// gen_theme_errors.mjs -- tests/pi/tui/theme_errors.json: the message pi 1.2.0's own validateThemeJson (theme-schema.ts) throws for
// themes that break its schema (additionalProperties false on the root, colors and export; appearance; name), made from pi's dark.json.
//   node tools/pi_oracle/gen_theme_errors.mjs
import { readFileSync, writeFileSync, realpathSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { caDir } from "./pin.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const pkg = caDir;
const dist = join(pkg, "dist", "modes", "interactive", "theme");
const { validateThemeJson } = await import(pathToFileURL(join(dist, "theme-schema.js")).href);
const dark = JSON.parse(readFileSync(join(dist, "dark.json"), "utf8"));
const cases = {
	ok_schema: (t) => { t.$schema = "https://pi.dev/schemas/theme.schema.json"; },
	ok_appearance: (t) => { t.appearance = "light"; },
	root_extra_key: (t) => { t.author = "me"; },
	colors_extra_key: (t) => { t.colors.sparkle = "#ffffff"; },
	export_extra_key: (t) => { t.export = { pageBg: "#000000", frame: "#111111" }; },
	bad_appearance: (t) => { t.appearance = "grey"; },
	combo: (t) => { t.author = "me"; t.zeta = 1; t.colors.sparkle = "#ffffff"; t.colors.aaa = 2; t.export = { frame: "#111111" }; t.appearance = "grey"; },
	missing_and_extra: (t) => { delete t.colors.accent; delete t.colors.muted; t.author = "me"; t.colors.sparkle = "#fff"; },
	name_slash: (t) => { t.name = "a/b"; },
};
const out = {};
for (const [k, f] of Object.entries(cases)) {
	const t = JSON.parse(JSON.stringify(dark));
	t.name = "mine";
	f(t);
	let msg = "";
	try { validateThemeJson("mine", t); } catch (e) { msg = e.message; }
	out[k] = { theme: t, error: msg };
}
writeFileSync(join(here, "..", "..", "tests", "pi", "tui", "theme_errors.json"), JSON.stringify(out, null, 1) + "\n");
console.log("[OK] " + Object.keys(out).length + " cases");
