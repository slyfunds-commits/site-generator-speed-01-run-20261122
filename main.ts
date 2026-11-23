// Minimal driver: one native call does everything (read, parse, render, write).
// solve.js is not used on this path; the entrypoint stays main.ts as required.
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const eng = require('./engine.node');
const inDir = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const outDir = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';
eng.runFiles(inDir + '/cases.json', outDir + '/answers.json');
