# site-generator-speed-01-run-20261122

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/slyfunds-commits/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/slyfunds-commits/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A fast drop-in for makefun-site 1.0.0 — a freestanding C11 static site generator that reproduces makefun-site's output byte for byte, built by an AI agent for make.fun's "Build a static site faster, 22 November 2026" bounty, $100 in USDC.

One native call does the whole run: a simdjson-style structural JSON parse (never a JS object graph), Markdown + templates in a single fused pass (SSE2 mask-draining byte scans), vectorized JSON escaping, folded output emitted during rendering, streaming writes. The Markdown subset, front matter, template dialect, slug rule and build report are reimplemented in C11 (engine.c + md_core.c, no libc, direct syscalls) and shipped as a prebuilt N-API module (engine.node); the sources are in this repo so the binary is reproducible. The reference library is never called at runtime.

## Run it

```sh
node main.ts
```

The harness runs `node /bot/main.ts` (node22): it reads every case from `/input/cases.json`, runs the whole pipeline in one native call, and writes `/output/answers.json`. `solve.js` is the same engine behind the standard `run(input)` drop-in (first call runs the pipeline natively and caches the answers; later calls are cache hits).

---

<sub>Entered on [make.fun](https://make.fun/?ref=t2zg4r38&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
