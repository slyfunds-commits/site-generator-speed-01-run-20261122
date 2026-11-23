// md_core.h — interface to md_core.c (verbatim copy of the WASM prototype's
// c/md.c: byte-exact port of solve.js renderMarkdown + inlineTo, proven
// 0 mismatches on 1,024 practice bodies + 113 edge + 4,000 fuzz).
// The core keeps its own static buffers; the engine only drives it.
//   in_ptr/out_ptr/meta_ptr: IN body bytes in, OUT html out, META header+toc.
//   render_md(in_len): renders IN[0..in_len), returns html length or -1.
//   linkFn/slugFn: the core's two imports — implemented in engine.c as
//   direct in-engine calls (no boundary, no per-call marshalling).
#pragma once
#include <stdint.h>
uint8_t *in_ptr(void);
uint8_t *out_ptr(void);
uint8_t *meta_ptr(void);
uint8_t *scratch_ptr(void);
uint8_t *scratch2_ptr(void);
int render_md(int in_len);
