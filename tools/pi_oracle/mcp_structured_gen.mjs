// What pi 1.2.0's convertMcpResult returns beside the model content: details and structuredContent (the CallToolResult
// without _meta), for each result of tests/pi/mcp/results.json plus a result whose text is cut and saved. Written to
// tests/pi/mcp/structured.expected, the file tests/test_pi_mcp.c compares mcp_result_ex against.
//   node tools/pi_oracle/mcp_structured_gen.mjs      (the pinned install: tools/pi_oracle/pin.mjs)
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { caDir } from "./pin.mjs";
const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const T = await import(pathToFileURL(join(caDir, "dist/extensions/mcp/tools.js")).href);
const dir = join(root, "tests", "pi", "mcp");
// results.json, then numbers.json: results with numbers as JS prints them (1.0 -> 1, 1e21, 1e-7, -0 -> 0, past the range -> null)
const results = { ...JSON.parse(readFileSync(join(dir, "results.json"), "utf8")), ...JSON.parse(readFileSync(join(dir, "numbers.json"), "utf8")) };
results.long = { content: [{ type: "text", text: "x".repeat(25000) }], _meta: { progressToken: 1 } };
const out = [];
for (const [name, res] of Object.entries(results)) {
	// pi-mcp's callTool: a result without content gets content [] (client.js validateCallToolResult)
	const r = res.content === undefined ? { ...res, content: [] } : res;
	const c = await T.convertMcpResult("fake", name, r, { saveOutput: async (_d, ext) => `SAVED${ext}` });
	const { content: _c, ...rest } = c;
	// a result with no content has its structuredContent printed as the text (JSON.stringify(sc, null, 2)): the text is part of the golden
	if (name.startsWith("numbers")) {
		out.push(`== ${name}`, JSON.stringify({ ...rest, content: c.content }));
		continue;
	}
	// the cut text is in structuredContent whole (25 KB): the golden keeps its length
	if (name === "long") out.push(`== ${name}`, JSON.stringify({ details: rest.details, structuredBytes: JSON.stringify(rest.structuredContent).length }));
	else out.push(`== ${name}`, JSON.stringify(rest));
}
writeFileSync(join(dir, "structured.expected"), out.join("\n") + "\n");
