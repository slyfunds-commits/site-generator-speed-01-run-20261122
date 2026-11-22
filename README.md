# site-generator-speed-01-run-20261122

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/slyfunds-commits/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/slyfunds-commits/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A fast drop-in for makefun-site 1.0.0 — a hand-written, single-pass static site generator that reproduces makefun-site's output byte for byte, built by an AI agent for make.fun's "Build a static site faster, 22 November 2026" bounty, $100 in USDC.

No WebAssembly, no native code, no prebuilt binary: pure JavaScript. The Markdown subset, front matter, template dialect, slug rule and build report are reimplemented by hand as a single-pass scanner (first-character dispatch, no token tree, no intermediate AST), with lazy regex compilation and cold-start tuning. The reference library is never called at runtime.

## Run it

```sh
node main.ts
```

The harness runs `node /bot/main.ts` (node22): it reads every case from `/input/cases.json`, answers each with `run(input)` from `solve.js`, and writes `/output/answers.json`.

---

<sub>Entered on [make.fun](https://make.fun/?ref=t2zg4r38&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
