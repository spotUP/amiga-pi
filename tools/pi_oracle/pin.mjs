// pin.mjs -- the one place that says which pi the oracle generators read.
//   import { oracleDir, caDir, npmRoot, piSrc } from "./pin.mjs"     (node)
//   node tools/pi_oracle/pin.mjs [npm|ca|src]                        (python, sh: prints the path)
// The pin is pi 1.2.0 (tag v1.2.0, 06a46693): npm install --prefix vendor/pi-1.2.0 @earendil-works/pi-coding-agent@1.2.0
// This checkout's vendor/, else the main checkout's when this is build/wt-<name>. PI_ORACLE_DIR names another
// install (the tests point it at vendor/pi-1.1.0 to prove the refusal). Exit 3: no install; exit 2: not 1.2.0.
import { existsSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export const PINNED = "1.2.0";
const here = dirname(fileURLToPath(import.meta.url));
const has = (c) => existsSync(join(c, "node_modules", "@earendil-works", "pi-coding-agent", "package.json"));
function find() {
	if (process.env.PI_ORACLE_DIR) return resolve(process.env.PI_ORACLE_DIR);
	for (const root of ["../..", "../../../.."]) {
		const c = resolve(here, root, "vendor", "pi-1.2.0");
		if (has(c)) return c;
	}
	console.error("pin: no pinned pi: npm install --prefix vendor/pi-1.2.0 @earendil-works/pi-coding-agent@1.2.0");
	process.exit(3);
}
export const oracleDir = find();
export const npmRoot = join(oracleDir, "node_modules");
export const caDir = join(npmRoot, "@earendil-works", "pi-coding-agent");
export const caVersion = JSON.parse(readFileSync(join(caDir, "package.json"), "utf8")).version;
if (caVersion !== PINNED) {
	console.error(`pin: the oracle is pi-coding-agent ${caVersion}, not the pinned ${PINNED}; refusing to write goldens`);
	process.exit(2);
}
if (process.env.PI_ORACLE_VERSION_ONLY === "1" && process.argv[1] === fileURLToPath(import.meta.url)) process.exit(0);
// pi's TypeScript source at the same tag (vendor/pi-src, a checkout of v1.2.0), for the generators that cut text
// out of source (the npm package ships dist only). Checked against the pinned version.
export function piSrc() {
	for (const root of ["../..", "../../../.."]) {
		const c = resolve(here, root, "vendor", "pi-src");
		const pj = join(c, "packages", "coding-agent", "package.json");
		if (existsSync(pj)) {
			const v = JSON.parse(readFileSync(pj, "utf8")).version;
			if (v !== PINNED) { console.error(`pin: vendor/pi-src is ${v}, not ${PINNED}`); process.exit(2); }
			return c;
		}
	}
	console.error("pin: no vendor/pi-src (git clone of earendil-works/pi at tag v1.2.0)");
	process.exit(3);
}
if (process.argv[1] === fileURLToPath(import.meta.url)) {
	const what = process.argv[2] ?? "npm";
	console.log(what === "ca" ? caDir : what === "src" ? piSrc() : what === "dir" ? oracleDir : npmRoot);
}
