// makefun-site 1.0.0, the house entry, make.fun's own reference program: the harness runs `node /bot/main.ts` (node22). It reads every case from /input/cases.json (a JSON
// array of {id, input}), answers each one alone with run() from solve.js, and writes /output/answers.json: a JSON
// array of the answers, one per case and in the cases' order. An entry keeps this driver and replaces solve.js with a
// faster module whose run(input) returns the same answer for every input. Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

// V8's incremental marking off, set before solve.js is imported so all of its work runs under it (the rules: the house
// baseline runs with --no-incremental-marking, and an entry may tune V8's flags too). TurboFan's optimiser is off
// as well: this entry runs three cases once each, so the optimiser's background compilation costs more CPU than
// its faster code ever repays on so short a run; Sparkplug carries the work instead. The young generation is
// roomier, so the collector runs less often.
// NOTE: --single-threaded was REMOVED (round 2): on the grader's node22 (V8 12.4) setting it via
// setFlagsFromString after isolate init trips a CppHeap CHECK (concurrent_marking_handle_) at the next GC and
// kills the process with SIGTRAP (exit 133) — round 1 died exactly that way. Bisected on node 22.21.1: the
// flag alone reproduces 133, everything else exits 0. It was worth ~nothing locally anyway (neutral vs v5).
setFlagsFromString('--no-incremental-marking --no-turbofan --max-semi-space-size=64');

/** solve.js, imported by its URL (a .js module carries no types) and typed by its contract. */
const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const cases = JSON.parse(readFileSync(join(INPUT_DIR, 'cases.json'), 'utf8')) as { id: string; input: unknown }[];
const answers: unknown[] = [];
for (const c of cases) answers.push(run(c.input));
writeFileSync(join(OUTPUT_DIR, 'answers.json'), stringifyAnswers(answers));

/**
 * The answers as JSON. Each answer is a plain object of path -> text, so
 * the array is built by hand with one JSON.stringify per string instead of
 * the generic serializer's slow first pass over a multi-megabyte object
 * (measured: ~48 ms against ~90 ms cold for the same bytes, identical
 * output). Anything that is not that shape falls back to JSON.stringify.
 */
function stringifyAnswers(answers: unknown): string {
  if (!Array.isArray(answers)) return JSON.stringify(answers);
  let out = '[';
  for (let i = 0; i < answers.length; i++) {
    const obj = answers[i] as Record<string, unknown>;
    if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) return JSON.stringify(answers);
    if (i > 0) out += ',';
    out += '{';
    let first = true;
    for (const k of Object.keys(obj)) {
      const v = obj[k];
      if (typeof v !== 'string') return JSON.stringify(answers);
      if (!first) out += ',';
      first = false;
      out += JSON.stringify(k);
      out += ':';
      out += JSON.stringify(v);
    }
    out += '}';
  }
  return `${out}]`;
}
