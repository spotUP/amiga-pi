// gen_theme.mjs -- tests/pi/tui/system_theme.txt: pi 1.0.4's own `system` theme as it draws on a terminal that
// reports no colours (generateSystemThemeColors with nothing), one line per role: name, ANSI slot or "-", dim flag.
//   node tools/pi_oracle/gen_theme.mjs
import { writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { execSync } from "node:child_process";
import { caDir, npmRoot } from "./pin.mjs";

const root = join(caDir, "dist/modes/interactive/theme");
const { generateSystemThemeColors } = await import(pathToFileURL(join(root, "system-theme.js")).href);
const t = generateSystemThemeColors({});
const out = [];
for (const [k, v] of Object.entries(t.colors)) out.push(`${k} ${v === "" ? "-" : v} ${t.dim.includes(k) ? "dim" : "-"}`);
writeFileSync(join(dirname(fileURLToPath(import.meta.url)), "..", "..", "tests", "pi", "tui", "system_theme.txt"), out.join("\n") + "\n");
console.log(`[OK] ${out.length} roles`);

// pi's own dark.json and light.json (copied) and what pi's parseColor / colorToHex make of each role: tests/pi/tui/theme_<name>.txt
import { copyFileSync, readFileSync } from "node:fs";
const tui = await import(pathToFileURL(join(npmRoot, "@earendil-works/pi-tui/dist/colors.js")).href);
const out2 = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "tests", "pi", "tui");
for (const name of ["dark", "light"]) {
	copyFileSync(join(root, name + ".json"), join(out2, name + ".json"));
	const j = JSON.parse(readFileSync(join(root, name + ".json"), "utf8"));
	const resolve = (v, seen = 0) => (typeof v === "string" && j.vars && v in j.vars && seen < 10 ? resolve(j.vars[v], seen + 1) : v);
	const lines = [];
	for (const [k, v] of Object.entries(j.colors)) {
		const r = resolve(v);
		lines.push(`${k} ${r === "" ? "-" : typeof r === "number" ? "#" + r : tui.colorToHex(tui.parseColor(r))}`);
	}
	writeFileSync(join(out2, `theme_${name}.txt`), lines.join("\n") + "\n");
}
const samples = ["oklch(70% 0.1 150)", "oklch(100% 0.3 150)", "oklch(0.5 0.4 30)", "okhsl(180 50% 50%)", "okhsl(295deg 80% 30%)", "okhsl(0 0% 100%)", "okhsl(20 100% 62%)", "#abc", "#ff8800"];
writeFileSync(join(out2, "colors.txt"), samples.map((s) => `${s}\t${tui.colorToHex(tui.parseColor(s))}`).join("\n") + "\n");
console.log("[OK] themes and colours");
