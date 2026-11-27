// Bundles a SolidJS app for the device: JSX through babel-preset-solid in
// universal mode (rendering through solid-lvgl.js), then esbuild into one
// ES module. "lvgl" stays an import: the firmware provides it.
//
//   npm install            (once, in this folder)
//   node build.mjs app.jsx app.js
import { transformAsync } from "@babel/core";
import * as esbuild from "esbuild";
import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const solidPreset = createRequire(import.meta.url).resolve("babel-preset-solid");
const [entry, outfile] = process.argv.slice(2);
if (!entry || !outfile) {
    console.error("usage: node build.mjs app.jsx out.js");
    process.exit(2);
}

const solidJsx = {
    name: "solid-jsx",
    setup(build) {
        build.onLoad({ filter: /\.[jt]sx$/ }, async (args) => {
            const src = await readFile(args.path, "utf8");
            const out = await transformAsync(src, {
                filename: args.path,
                presets: [[solidPreset, { generate: "universal", moduleName: "solid-lvgl" }]],
            });
            return { contents: out.code, loader: "js" };
        });
    },
};

await esbuild.build({
    entryPoints: [entry],
    outfile,
    bundle: true,
    format: "esm",
    platform: "neutral",
    mainFields: ["module", "main"],
    conditions: ["browser"], // Solid's reactive build, not its server one
    external: ["lvgl"],
    alias: { "solid-lvgl": path.join(here, "solid-lvgl.js") },
    nodePaths: [path.join(here, "node_modules")], // apps can live anywhere
    target: "es2023",
    minify: true,
    legalComments: "none",
    plugins: [solidJsx],
});
