// site-generator-speed-01 round 7: freestanding C11 native engine (N-API).
// No libc: direct syscalls, own arena allocator, SSE2 scans. One file.
// Build: gcc -O3 -msse2 -fPIC -shared -nostdlib -I/usr/include/node -o engine.node engine.c
// Test:  gcc -O2 -DENGINE_TEST_MAIN -o engine_test engine.c  (standalone CLI)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <immintrin.h>
#include "slugtab.h"

#ifndef ENGINE_TEST_MAIN
#include <node_api.h>
#endif

// ---------------------------------------------------------------- syscalls (x86-64)
static long sys1(long n, long a) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r; }
static long sys2(long n, long a, long b) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory"); return r; }
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  // x86-64 syscall args: rax=n, rdi, rsi, rdx, r10, r8, r9. The 4th-6th
  // args MUST be pinned to r10/r8/r9 — plain "r" constraints let gcc pick
  // any register (and syscall clobbers rcx/r11), which scrambles the call.
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  long r;
  __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
  return r;
}
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_CLOSE 3
#define SYS_FSTAT 5
#define SYS_MMAP 9
#define SYS_MUNMAP 11
#define SYS_OPENAT 257
#define AT_FDCWD (-100)

typedef struct { long st_size; long _pad[13]; } KStat; // struct stat is bigger; we only need st_size at offset 48
// Real struct stat (x86-64): st_dev(0) st_ino(8) st_nlink(16) st_mode(24) st_uid(28) st_gid(32) st_pad0(36) st_rdev(40) st_size(48) ...
static long file_size(long fd) {
  char st[144];
  if (sys2(SYS_FSTAT, fd, (long)st) != 0) return -1;
  long sz; __builtin_memcpy(&sz, st + 48, 8);
  return sz;
}
static uint8_t *map_file(const char *path, size_t *len_out) {
  long fd = sys3(SYS_OPENAT, AT_FDCWD, (long)path, 0);
  if (fd < 0) return NULL;
  long sz = file_size(fd);
  if (sz < 0) { sys1(SYS_CLOSE, fd); return NULL; }
  uint8_t *p = (uint8_t *)sys6(SYS_MMAP, 0, sz > 0 ? sz : 1, 3 /* PROT_READ|WRITE */, 0x02 /* MAP_PRIVATE */, fd, 0);
  sys1(SYS_CLOSE, fd);
  if ((long)p < 0) return NULL;
  *len_out = (size_t)sz;
  return p;
}
static int write_all(long fd, const uint8_t *p, size_t n) {
  while (n > 0) {
    long w = sys3(SYS_WRITE, fd, (long)p, (long)n);
    if (w <= 0) return -1;
    p += w; n -= (size_t)w;
  }
  return 0;
}

// ---- step timers (always compiled; read by the benchsteps test command) ----
static uint64_t t_now_us(void) {
  long ts[2];
  if (sys2(228, 1, (long)ts) != 0) return 0;
  return (uint64_t)ts[0] * 1000000 + (uint64_t)(ts[1] / 1000);
}
static uint64_t g_step_us[16];
static int g_step_cur = -1;
static uint64_t g_step_t0;
#define STEP_BEGIN(n) do { g_step_cur = (n); g_step_t0 = t_now_us(); } while (0)
#define STEP_END() do { if (g_step_cur >= 0) g_step_us[g_step_cur] += t_now_us() - g_step_t0; g_step_cur = -1; } while (0)

// ---------------------------------------------------------------- arena
typedef struct { uint8_t *base, *cur, *end; } Arena;
static void arena_init(Arena *a, size_t cap) {
  uint8_t *p = (uint8_t *)sys6(SYS_MMAP, 0, (long)cap, 3, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
  a->base = a->cur = p; a->end = p + cap;
}
static void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (a->cur + n > a->end) {
    // grow: map another 64MB chunk and link via a fresh arena is complex;
    // instead over-allocate upfront (see arena_grow below). This path aborts.
    __builtin_trap();
  }
  void *p = a->cur; a->cur += n;
  return p;
}
static void arena_reset(Arena *a) { a->cur = a->base; }
static void arena_free(Arena *a) { sys2(SYS_MUNMAP, (long)a->base, (long)(a->end - a->base)); }

// ---------------------------------------------------------------- strings
typedef struct { const uint8_t *p; size_t len; } Str;
static Str str_from(const char *s) { Str r; r.p = (const uint8_t *)s; r.len = __builtin_strlen(s); return r; }
static int str_eq(Str a, Str b) { return a.len == b.len && (a.len == 0 || __builtin_memcmp(a.p, b.p, a.len) == 0); }
// UTF-16 code-unit iterator over a UTF-8 string (for JS code-unit order).
typedef struct { const uint8_t *p, *end; uint32_t pending; int has_pending; } U16Iter;
static uint32_t utf8_decode(const uint8_t **pp, const uint8_t *end);
static uint32_t u16_next(U16Iter *it) {
  if (it->has_pending) { it->has_pending = 0; return it->pending; }
  if (it->p >= it->end) return 0xFFFFFFFFu;
  uint32_t cp = utf8_decode(&it->p, it->end);
  if (cp >= 0x10000) {
    it->pending = 0xDC00u + ((cp - 0x10000u) & 0x3FFu);
    it->has_pending = 1;
    return 0xD800u + ((cp - 0x10000u) >> 10);
  }
  return cp;
}
static int str_cmp(Str a, Str b) {
  // code-unit order (UTF-16 code units) like JS.
  U16Iter ia = { a.p, a.p + a.len, 0, 0 }, ib = { b.p, b.p + b.len, 0, 0 };
  for (;;) {
    uint32_t ua = u16_next(&ia), ub = u16_next(&ib);
    if (ua == 0xFFFFFFFFu && ub == 0xFFFFFFFFu) return 0;
    if (ua == 0xFFFFFFFFu) return -1;
    if (ub == 0xFFFFFFFFu) return 1;
    if (ua != ub) return ua < ub ? -1 : 1;
  }
}

// UTF-8 decode one code point; advances *pp. Invalid bytes -> 0xFFFD, +1.
// WTF-8 surrogate sequences (ED A0..BF) decode to 0xD800..0xDFFF.
static uint32_t utf8_decode(const uint8_t **pp, const uint8_t *end) {
  const uint8_t *p = *pp;
  if (p >= end) return 0xFFFD;
  uint8_t c = *p;
  if (c < 0x80) { *pp = p + 1; return c; }
  if ((c & 0xE0) == 0xC0 && p + 1 < end) { *pp = p + 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0 && p + 2 < end) { *pp = p + 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
  if ((c & 0xF8) == 0xF0 && p + 3 < end) { *pp = p + 4; return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); }
  *pp = p + 1;
  return 0xFFFD;
}
// Encode cp as UTF-8 into out, return bytes written (surrogates -> WTF-8).
static int utf8_encode(uint32_t cp, uint8_t *out) {
  if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
  if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
  if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
  out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); return 4;
}

// ---------------------------------------------------------------- output buffer
typedef struct { uint8_t *buf; size_t len, cap; Arena *arena; } Buf;
static void buf_init(Buf *b, Arena *a, size_t cap) {
  b->buf = arena_alloc(a, cap); b->len = 0; b->cap = cap; b->arena = a;
}
static void buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra <= b->cap) return;
  size_t ncap = b->cap ? b->cap : 4096;
  while (ncap < b->len + extra) ncap *= 2;
  uint8_t *nb = arena_alloc(b->arena, ncap);
  __builtin_memcpy(nb, b->buf, b->len);
  b->buf = nb; b->cap = ncap;
}
static void buf_put(Buf *b, const void *p, size_t n) { buf_reserve(b, n); __builtin_memcpy(b->buf + b->len, p, n); b->len += n; }
static void buf_putc(Buf *b, uint8_t c) { buf_reserve(b, 1); b->buf[b->len++] = c; }
static void buf_puts(Buf *b, const char *s) { buf_put(b, s, __builtin_strlen(s)); }
static void buf_put_str(Buf *b, Str s) { buf_put(b, s.p, s.len); }

// ---------------------------------------------------------------- JSON parse (recursive descent over bytes, fills structs)
typedef struct { const uint8_t *p, *end; Arena *arena; } J;
static void j_ws(J *j) {
  // SIMD skip of JSON whitespace (space, tab, LF, CR), 16 bytes at a time.
  __m128i v20 = _mm_set1_epi8(' ');
  __m128i v09 = _mm_set1_epi8('\t');
  __m128i v0a = _mm_set1_epi8('\n');
  __m128i v0d = _mm_set1_epi8('\r');
  while (j->p + 16 <= j->end) {
    __m128i v = _mm_loadu_si128((const __m128i *)j->p);
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, v20),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v09),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v0a), _mm_cmpeq_epi8(v, v0d))));
    unsigned mask = (unsigned)_mm_movemask_epi8(m);
    if (mask == 0xFFFF) { j->p += 16; continue; }
    // first non-whitespace byte in this block
    j->p += (size_t)__builtin_ctz(~mask);
    return;
  }
  while (j->p < j->end) {
    uint8_t c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
    else break;
  }
}
static int j_lit(J *j, const char *lit) {
  size_t n = __builtin_strlen(lit);
  if ((size_t)(j->end - j->p) < n || __builtin_memcmp(j->p, lit, n) != 0) return 0;
  j->p += n;
  return 1;
}
// Parse a JSON string; returns unescaped UTF-8 (arena). Fast path: no
// backslash -> slice of the input (no copy).
static Str j_string(J *j) {
  // assumes *p == '"'
  j->p++;
  const uint8_t *start = j->p;
  const uint8_t *p = j->p;
  // SIMD fast scan for '"' or '\\' (16 bytes at a time); the scalar tail
  // covers the last <16 bytes. Zero-copy for clean strings (a view into the
  // input); the slow path below only runs when an escape is present.
  __m128i vq = _mm_set1_epi8('"');
  __m128i vb = _mm_set1_epi8('\\');
  while (p + 16 <= j->end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vb));
    unsigned mask = (unsigned)_mm_movemask_epi8(m);
    if (!mask) { p += 16; continue; }
    p += (size_t)__builtin_ctz(mask);
    goto jstr_found;
  }
  while (p < j->end && *p != '"' && *p != '\\') p++;
jstr_found:
  if (p < j->end && *p == '"') {
    Str r = { start, (size_t)(p - start) };
    j->p = p + 1;
    return r;
  }
  // slow path: escapes present; copy + unescape into the arena.
  // Find the closing quote first: the raw span is an upper bound on the
  // unescaped length. (Allocating to end-of-input here exhausted the arena:
  // every escaped string cost ~the whole remaining file.)
  const uint8_t *q = start;
  while (q < j->end) {
    if (*q == '\\') { q += 2; continue; }
    if (*q == '"') break;
    q++;
  }
  uint8_t *out = arena_alloc(j->arena, (size_t)(q - start) + 8);
  uint8_t *o = out;
  p = start;
  while (p < q && *p != '"') {
    uint8_t c = *p;
    if (c != '\\') { *o++ = c; p++; continue; }
    p++;
    uint8_t e = *p++;
    switch (e) {
      case '"': *o++ = '"'; break;
      case '\\': *o++ = '\\'; break;
      case '/': *o++ = '/'; break;
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case 'n': *o++ = '\n'; break;
      case 'r': *o++ = '\r'; break;
      case 't': *o++ = '\t'; break;
      case 'u': {
        uint32_t cp = 0;
        for (int k = 0; k < 4; k++) {
          uint8_t h = *p++;
          cp = (cp << 4) | (h <= '9' ? (uint32_t)(h - '0') : (h <= 'F' ? (uint32_t)(h - 'A' + 10) : (uint32_t)(h - 'a' + 10)));
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= j->end && p[0] == '\\' && p[1] == 'u') {
          uint32_t lo = 0;
          int ok = 1;
          for (int k = 0; k < 4; k++) {
            uint8_t h = p[2 + k];
            if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') || (h >= 'a' && h <= 'f'))) { ok = 0; break; }
            lo = (lo << 4) | (h <= '9' ? (uint32_t)(h - '0') : (h <= 'F' ? (uint32_t)(h - 'A' + 10) : (uint32_t)(h - 'a' + 10)));
          }
          if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        o += utf8_encode(cp, o);
        break;
      }
      default: *o++ = e; break; // invalid JSON; copy through
    }
  }
  j->p = p + 1; // past closing quote
  Str r = { out, (size_t)(o - out) };
  return r;
}
static int64_t j_int(J *j) {
  int neg = 0;
  if (j->p < j->end && *j->p == '-') { neg = 1; j->p++; }
  int64_t v = 0;
  while (j->p < j->end && *j->p >= '0' && *j->p <= '9') { v = v * 10 + (*j->p - '0'); j->p++; }
  // skip fraction/exponent (not expected in this schema, but be safe)
  if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')) {
    while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) j->p++;
  }
  return neg ? -v : v;
}
static void j_skip(J *j) {
  j_ws(j);
  if (j->p >= j->end) return;
  uint8_t c = *j->p;
  if (c == '"') { (void)j_string(j); return; }
  if (c == '{') {
    j->p++;
    j_ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return; }
    for (;;) {
      j_ws(j);
      if (j->p < j->end && *j->p == '"') (void)j_string(j);
      j_ws(j);
      if (j->p < j->end && *j->p == ':') j->p++;
      j_skip(j);
      j_ws(j);
      if (j->p < j->end && *j->p == ',') { j->p++; continue; }
      if (j->p < j->end && *j->p == '}') { j->p++; return; }
      return;
    }
  }
  if (c == '[') {
    j->p++;
    j_ws(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return; }
    for (;;) {
      j_skip(j);
      j_ws(j);
      if (j->p < j->end && *j->p == ',') { j->p++; continue; }
      if (j->p < j->end && *j->p == ']') { j->p++; return; }
      return;
    }
  }
  if (c == 't') { j_lit(j, "true"); return; }
  if (c == 'f') { j_lit(j, "false"); return; }
  if (c == 'n') { j_lit(j, "null"); return; }
  (void)j_int(j);
}

// ---------------------------------------------------------------- JSON emit (escape like JSON.stringify)
// SSE2 mask-draining scan: bytes needing escape are <0x20, '"' (0x22),
// '\\' (0x5C), or 0xED (WTF-8 lone-surrogate lead).
static const char HEXD[] = "0123456789abcdef";
static void json_emit_str(Buf *b, Str s) {
  // Fast path: reserve room for the string + typical escape expansion (25%)
  // and write through a raw cursor -- no per-hit buf_put calls. The answers
  // carry ~280k escapes (every \" and \n in the html), and each buf_put was
  // a function call + reserve check. On overflow (pathological escape
  // density) fall through to the slow buf_put loop for the remainder; the
  // opening quote is already written and p/run are the resume point.
  const uint8_t *p = s.p, *end = s.p + s.len;
  const uint8_t *run = p;
  buf_reserve(b, s.len + s.len / 4 + 18);
  uint8_t *dst = b->buf + b->len;
  uint8_t *dst_end = b->buf + b->cap;
  __m128i v22 = _mm_set1_epi8(0x22);
  __m128i v5c = _mm_set1_epi8(0x5C);
  __m128i ved = _mm_set1_epi8((char)0xED);
  // Unsigned byte < 0x20 via the XOR-0x80 bias trick: _mm_cmplt_epi8 is
  // SIGNED, so a plain compare would also flag every byte >= 0x80
  // (non-ASCII UTF-8) as "needs escape" and corrupt it. (This was a real
  // bug: 1011/1024 practice bodies mismatched before the fix.)
  __m128i vbias = _mm_set1_epi8((char)0x80);
  __m128i vlt = _mm_set1_epi8((char)0xA0); // 0x20 ^ 0x80
  *dst++ = '"';
  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_or_si128(_mm_cmplt_epi8(_mm_xor_si128(v, vbias), vlt),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v22),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v5c), _mm_cmpeq_epi8(v, ved))));
    unsigned mask = (unsigned)_mm_movemask_epi8(m);
    if (!mask) { p += 16; continue; }
    unsigned bit = __builtin_ctz(mask);
    const uint8_t *hit = p + bit;
    size_t runlen = (size_t)(hit - run);
    if (dst + runlen + 6 > dst_end) goto emit_slow;
    if (runlen) { __builtin_memcpy(dst, run, runlen); dst += runlen; }
    uint8_t c = *hit;
    if (c == '"') { dst[0] = '\\'; dst[1] = '"'; dst += 2; }
    else if (c == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
    else if (c == '\b') { dst[0] = '\\'; dst[1] = 'b'; dst += 2; }
    else if (c == '\f') { dst[0] = '\\'; dst[1] = 'f'; dst += 2; }
    else if (c == '\n') { dst[0] = '\\'; dst[1] = 'n'; dst += 2; }
    else if (c == '\r') { dst[0] = '\\'; dst[1] = 'r'; dst += 2; }
    else if (c == '\t') { dst[0] = '\\'; dst[1] = 't'; dst += 2; }
    else if (c < 0x20) {
      dst[0] = '\\'; dst[1] = 'u'; dst[2] = '0'; dst[3] = '0';
      dst[4] = (uint8_t)HEXD[c >> 4]; dst[5] = (uint8_t)HEXD[c & 15]; dst += 6;
    } else { // 0xED: WTF-8 lone surrogate ED A0..BF xx -> \uDXXX
      if (hit + 2 < end && (hit[1] & 0xE0) == 0xA0) {
        uint32_t cp = ((hit[1] & 0xF0) == 0xA0 ? 0xD800 : 0xDC00) + ((hit[1] & 0x0F) << 6) + (hit[2] & 0x3F);
        dst[0] = '\\'; dst[1] = 'u';
        dst[2] = (uint8_t)HEXD[(cp >> 12) & 15]; dst[3] = (uint8_t)HEXD[(cp >> 8) & 15];
        dst[4] = (uint8_t)HEXD[(cp >> 4) & 15]; dst[5] = (uint8_t)HEXD[cp & 15]; dst += 6;
        p = hit + 3; run = p;
        continue;
      }
      *dst++ = c;
      p = hit + 1; run = p;
      continue;
    }
    p = hit + 1; run = p;
  }
  while (p < end) {
    uint8_t c = *p;
    if (c < 0x20 || c == '"' || c == '\\' || c == 0xED) {
      size_t runlen = (size_t)(p - run);
      if (dst + runlen + 6 > dst_end) goto emit_slow;
      if (runlen) { __builtin_memcpy(dst, run, runlen); dst += runlen; }
      if (c == '"') { dst[0] = '\\'; dst[1] = '"'; dst += 2; }
      else if (c == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
      else if (c == '\b') { dst[0] = '\\'; dst[1] = 'b'; dst += 2; }
      else if (c == '\f') { dst[0] = '\\'; dst[1] = 'f'; dst += 2; }
      else if (c == '\n') { dst[0] = '\\'; dst[1] = 'n'; dst += 2; }
      else if (c == '\r') { dst[0] = '\\'; dst[1] = 'r'; dst += 2; }
      else if (c == '\t') { dst[0] = '\\'; dst[1] = 't'; dst += 2; }
      else if (c < 0x20) {
        dst[0] = '\\'; dst[1] = 'u'; dst[2] = '0'; dst[3] = '0';
        dst[4] = (uint8_t)HEXD[c >> 4]; dst[5] = (uint8_t)HEXD[c & 15]; dst += 6;
      } else if (p + 2 < end && (p[1] & 0xE0) == 0xA0) {
        uint32_t cp = ((p[1] & 0xF0) == 0xA0 ? 0xD800 : 0xDC00) + ((p[1] & 0x0F) << 6) + (p[2] & 0x3F);
        dst[0] = '\\'; dst[1] = 'u';
        dst[2] = (uint8_t)HEXD[(cp >> 12) & 15]; dst[3] = (uint8_t)HEXD[(cp >> 8) & 15];
        dst[4] = (uint8_t)HEXD[(cp >> 4) & 15]; dst[5] = (uint8_t)HEXD[cp & 15]; dst += 6;
        p += 3; run = p;
        continue;
      } else *dst++ = c;
      p++; run = p;
    } else p++;
  }
  {
    size_t runlen = (size_t)(end - run);
    if (dst + runlen + 1 > dst_end) goto emit_slow;
    if (runlen) { __builtin_memcpy(dst, run, runlen); dst += runlen; }
    *dst++ = '"';
    b->len = (size_t)(dst - b->buf);
    return;
  }
emit_slow:
  // Commit what the fast path wrote, then continue with the buf_put loop
  // from (p, run). The opening quote is already emitted.
  b->len = (size_t)(dst - b->buf);
  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_or_si128(_mm_cmplt_epi8(_mm_xor_si128(v, vbias), vlt),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v22),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v5c), _mm_cmpeq_epi8(v, ved))));
    unsigned mask = (unsigned)_mm_movemask_epi8(m);
    if (!mask) { p += 16; continue; }
    unsigned bit = __builtin_ctz(mask);
    const uint8_t *hit = p + bit;
    if (hit > run) buf_put(b, run, (size_t)(hit - run));
    uint8_t c = *hit;
    if (c == '"') buf_puts(b, "\\\"");
    else if (c == '\\') buf_puts(b, "\\\\");
    else if (c == '\b') buf_puts(b, "\\b");
    else if (c == '\f') buf_puts(b, "\\f");
    else if (c == '\n') buf_puts(b, "\\n");
    else if (c == '\r') buf_puts(b, "\\r");
    else if (c == '\t') buf_puts(b, "\\t");
    else if (c < 0x20) { char e[7] = { '\\', 'u', '0', '0', HEXD[c >> 4], HEXD[c & 15], 0 }; buf_put(b, e, 6); }
    else if (hit + 2 < end && (hit[1] & 0xE0) == 0xA0) {
      uint32_t cp = ((hit[1] & 0xF0) == 0xA0 ? 0xD800 : 0xDC00) + ((hit[1] & 0x0F) << 6) + (hit[2] & 0x3F);
      char e[7] = { '\\', 'u', HEXD[(cp >> 12) & 15], HEXD[(cp >> 8) & 15], HEXD[(cp >> 4) & 15], HEXD[cp & 15], 0 };
      buf_put(b, e, 6);
      p = hit + 3; run = p;
      continue;
    } else buf_putc(b, c);
    p = hit + 1; run = p;
  }
  while (p < end) {
    uint8_t c = *p;
    if (c < 0x20 || c == '"' || c == '\\' || c == 0xED) {
      if (p > run) buf_put(b, run, (size_t)(p - run));
      if (c == '"') buf_puts(b, "\\\"");
      else if (c == '\\') buf_puts(b, "\\\\");
      else if (c == '\b') buf_puts(b, "\\b");
      else if (c == '\f') buf_puts(b, "\\f");
      else if (c == '\n') buf_puts(b, "\\n");
      else if (c == '\r') buf_puts(b, "\\r");
      else if (c == '\t') buf_puts(b, "\\t");
      else if (c < 0x20) { char e[7] = { '\\', 'u', '0', '0', HEXD[c >> 4], HEXD[c & 15], 0 }; buf_put(b, e, 6); }
      else if (p + 2 < end && (p[1] & 0xE0) == 0xA0) {
        uint32_t cp = ((p[1] & 0xF0) == 0xA0 ? 0xD800 : 0xDC00) + ((p[1] & 0x0F) << 6) + (p[2] & 0x3F);
        char e[7] = { '\\', 'u', HEXD[(cp >> 12) & 15], HEXD[(cp >> 8) & 15], HEXD[(cp >> 4) & 15], HEXD[cp & 15], 0 };
        buf_put(b, e, 6);
        p += 3; run = p;
        continue;
      } else buf_putc(b, c);
      p++; run = p;
    } else p++;
  }
  if (end > run) buf_put(b, run, (size_t)(end - run));
  buf_putc(b, '"');
}

// ---------------------------------------------------------------- text
static void esc_html_buf(Buf *b, Str s) {
  // escape & < > " '  (SSE2 scan for the set)
  const uint8_t *p = s.p, *end = s.p + s.len;
  const uint8_t *run = p;
  __m128i v26 = _mm_set1_epi8(0x26);
  __m128i v3c = _mm_set1_epi8(0x3C);
  __m128i v3e = _mm_set1_epi8(0x3E);
  __m128i v22 = _mm_set1_epi8(0x22);
  __m128i v27 = _mm_set1_epi8(0x27);
  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, v26),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v3c),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v3e),
                  _mm_or_si128(_mm_cmpeq_epi8(v, v22), _mm_cmpeq_epi8(v, v27)))));
    unsigned mask = (unsigned)_mm_movemask_epi8(m);
    if (!mask) { p += 16; continue; }
    unsigned bit = __builtin_ctz(mask);
    const uint8_t *hit = p + bit;
    if (hit > run) buf_put(b, run, (size_t)(hit - run));
    switch (*hit) {
      case '&': buf_puts(b, "&amp;"); break;
      case '<': buf_puts(b, "&lt;"); break;
      case '>': buf_puts(b, "&gt;"); break;
      case '"': buf_puts(b, "&quot;"); break;
      default: buf_puts(b, "&#39;"); break;
    }
    p = hit + 1; run = p;
  }
  while (p < end) {
    uint8_t c = *p;
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') {
      if (p > run) buf_put(b, run, (size_t)(p - run));
      switch (c) {
        case '&': buf_puts(b, "&amp;"); break;
        case '<': buf_puts(b, "&lt;"); break;
        case '>': buf_puts(b, "&gt;"); break;
        case '"': buf_puts(b, "&quot;"); break;
        default: buf_puts(b, "&#39;"); break;
      }
      p++; run = p;
    } else p++;
  }
  if (end > run) buf_put(b, run, (size_t)(end - run));
}

// slug contribution lookup: binary search in SLUG_TAB (sorted by cp).
static const char *slug_lookup(uint32_t cp, uint8_t *len_out) {
  int lo = 0, hi = SLUG_TAB_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t m = SLUG_TAB[mid].cp;
    if (m == cp) { *len_out = SLUG_TAB[mid].len; return SLUG_BLOB + SLUG_TAB[mid].off; }
    if (m < cp) lo = mid + 1; else hi = mid - 1;
  }
  *len_out = 0;
  return NULL;
}
// The slug of text (NFKD, marks dropped, a-z0-9, runs -> '-', max 64,
// trailing '-' dropped, "untitled" when empty). Result in arena.
static Str slugify(Arena *a, Str text) {
  uint8_t *out = arena_alloc(a, text.len + 8);
  size_t n = 0;
  int gap = 0;
  const uint8_t *p = text.p, *end = text.p + text.len;
  while (p < end) {
    uint32_t cp = utf8_decode(&p, end);
    if (cp < 0x80) {
      char c = (char)cp;
      char keep = 0;
      if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
      if (!keep) { gap = n > 0; continue; }
      if (gap) out[n++] = '-';
      gap = 0;
      out[n++] = (uint8_t)keep;
      continue;
    }
    uint8_t clen = 0;
    const char *contrib = slug_lookup(cp, &clen);
    if (clen == 0) { gap = n > 0; continue; }
    if (gap) out[n++] = '-';
    gap = 0;
    for (int k = 0; k < clen; k++) out[n++] = (uint8_t)contrib[k];
  }
  if (n > 64) n = 64;
  while (n > 0 && out[n - 1] == '-') n--;
  if (n == 0) { Str r = str_from("untitled"); return r; }
  Str r = { out, n };
  return r;
}

static const char *MONTHS[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
// "2026-11-09" -> "9 November 2026" (from the string alone).
static Str long_date(Arena *a, Str date) {
  // date is YYYY-MM-DD
  int d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  int m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
  uint8_t *out = arena_alloc(a, 32);
  int n = 0;
  // d as decimal
  char tmp[4]; int tn = 0;
  int dd = d;
  if (dd == 0) tmp[tn++] = '0';
  while (dd > 0) { tmp[tn++] = (char)('0' + dd % 10); dd /= 10; }
  while (tn > 0) out[n++] = (uint8_t)tmp[--tn];
  out[n++] = ' ';
  const char *mo = MONTHS[m - 1];
  while (*mo) out[n++] = (uint8_t)*mo++;
  out[n++] = ' ';
  for (int k = 0; k < 4; k++) out[n++] = date.p[k];
  Str r = { out, (size_t)n };
  return r;
}

// words of a body: runs of non-whitespace (JS \S). Whitespace per JS:
// 0x09-0x0D, 0x20, 0xA0, 0x1680, 0x2000-0x200A, 0x2028, 0x2029, 0x202F,
// 0x205F, 0x3000, 0xFEFF. (Unicode White_Space.)
static int is_js_ws(uint32_t cp) {
  if (cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20 ||
      cp == 0xA0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
      cp == 0x205F || cp == 0x3000 || cp == 0xFEFF) return 1;
  if (cp >= 0x2000 && cp <= 0x200A) return 1;
  return 0;
}
static long words_of_scalar(Str text, long n, int in_word) {
  const uint8_t *p = text.p, *end = text.p + text.len;
  while (p < end) {
    uint8_t c = *p;
    if (c < 0x80) {
      int ws = (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20);
      if (ws) in_word = 0;
      else if (!in_word) { in_word = 1; n++; }
      p++;
    } else {
      const uint8_t *q = p;
      uint32_t cp = utf8_decode(&q, end);
      if (is_js_ws(cp)) in_word = 0;
      else if (!in_word) { in_word = 1; n++; }
      p = q;
    }
  }
  return n;
}
static long words_of(Str text) {
  // SIMD fast path (pure ASCII): a word starts at a non-ws byte whose
  // previous byte was ws (or at position 0). ws bytes are 0x09-0x0D, 0x20.
  // Any byte >= 0x80 in a chunk falls back to the scalar path for the rest.
  long n = 0;
  const uint8_t *p = text.p, *end = text.p + text.len;
  int prev_ws = 1; // virtual ws before the string
  __m128i v20 = _mm_set1_epi8(0x20);
  __m128i v09 = _mm_set1_epi8(0x09);
  __m128i v0e = _mm_set1_epi8(0x0E); // < 0x0E means <= 0x0D (signed: 0x09-0x0D positive)
  __m128i v80 = _mm_set1_epi8((char)0x80);
  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    // non-ASCII present? (signed < 0 means byte >= 0x80)
    if (_mm_movemask_epi8(_mm_cmplt_epi8(v, _mm_setzero_si128()))) break;
    __m128i is20 = _mm_cmpeq_epi8(v, v20);
    __m128i ge09 = _mm_cmpgt_epi8(v, _mm_set1_epi8(0x08));
    __m128i lt0e = _mm_cmplt_epi8(v, v0e);
    __m128i isws = _mm_or_si128(is20, _mm_and_si128(ge09, lt0e));
    unsigned w = (unsigned)_mm_movemask_epi8(isws);
    // word starts: non-ws byte whose previous byte is ws
    unsigned prev = ((unsigned)w << 1) | (prev_ws ? 1u : 0u);
    unsigned starts = (~w) & prev & 0xFFFFu;
    // bit 0 of prev must use the byte BEFORE the chunk (prev_ws), which is
    // bit 0 of (w<<1)|prev_ws... note w<<1 shifts ws of byte i to bit i+1,
    // so bit i of prev = ws of byte i-1. Correct.
    n += __builtin_popcount(starts);
    prev_ws = (w >> 15) & 1;
    (void)v09; (void)v80;
    p += 16;
  }
  if (p < end) {
    Str rest = { p, (size_t)(end - p) };
    // prev_ws here is whether the last processed byte was ws; recompute
    // in_word from it: in_word = !prev_ws (if we processed >=1 byte) else 0
    int in_word = (p > text.p) ? !prev_ws : 0;
    n = words_of_scalar(rest, n, in_word);
  }
  return n;
}

// ---------------------------------------------------------------- hash maps (Str -> void*), open addressing
typedef struct { Str key; void *val; int used; } HEnt;
typedef struct { HEnt *ents; size_t cap, n; Arena *arena; } HMap;
static uint64_t fnv(Str s) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.len; i++) { h ^= s.p[i]; h *= 1099511628211ULL; }
  return h;
}
static void hmap_init(HMap *m, Arena *a, size_t cap) {
  m->cap = cap; m->n = 0; m->arena = a;
  m->ents = arena_alloc(a, cap * sizeof(HEnt));
  for (size_t i = 0; i < cap; i++) m->ents[i].used = 0;
}
static void hmap_grow(HMap *m) {
  size_t ncap = m->cap * 2;
  HEnt *ne = arena_alloc(m->arena, ncap * sizeof(HEnt));
  for (size_t i = 0; i < ncap; i++) ne[i].used = 0;
  for (size_t i = 0; i < m->cap; i++) {
    if (!m->ents[i].used) continue;
    size_t j = fnv(m->ents[i].key) & (ncap - 1);
    while (ne[j].used) j = (j + 1) & (ncap - 1);
    ne[j] = m->ents[i];
  }
  m->ents = ne; m->cap = ncap;
}
static void *hmap_get(HMap *m, Str k) {
  if (m->cap == 0) return NULL;
  size_t j = fnv(k) & (m->cap - 1);
  while (m->ents[j].used) {
    if (str_eq(m->ents[j].key, k)) return m->ents[j].val;
    j = (j + 1) & (m->cap - 1);
  }
  return NULL;
}
static int hmap_has(HMap *m, Str k) { return hmap_get(m, k) != NULL; }
static void hmap_put(HMap *m, Str k, void *v) {
  if ((m->n + 1) * 10 > m->cap * 7) hmap_grow(m);
  size_t j = fnv(k) & (m->cap - 1);
  while (m->ents[j].used) {
    if (str_eq(m->ents[j].key, k)) { m->ents[j].val = v; return; }
    j = (j + 1) & (m->cap - 1);
  }
  m->ents[j].used = 1; m->ents[j].key = k; m->ents[j].val = v; m->n++;
}
// A set of strings: map to (void*)1.
typedef HMap StrSet;
static void strset_add(StrSet *s, Str k) { hmap_put(s, k, (void *)1); }
static int strset_has(StrSet *s, Str k) { return hmap_has(s, k); }
// JS uniqueSlug: first free slug, then slug-2, slug-3, ...; adds the winner.
static Str unique_slug(Arena *a, Str slug, StrSet *taken) {
  if (!strset_has(taken, slug)) { strset_add(taken, slug); return slug; }
  for (int n = 2; ; n++) {
    char tmp[16]; int tn = 0, v = n;
    while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
    uint8_t *o = arena_alloc(a, slug.len + 1 + (size_t)tn);
    size_t m = 0;
    for (size_t i = 0; i < slug.len; i++) o[m++] = slug.p[i];
    o[m++] = '-';
    while (tn > 0) o[m++] = (uint8_t)tmp[--tn];
    Str cand = { o, m };
    if (!strset_has(taken, cand)) { strset_add(taken, cand); return cand; }
  }
}

// ---------------------------------------------------------------- input model
typedef struct { Str title, base_url; int64_t per_page, recent; } Site;
typedef struct { Str name, src; } RawTemplate;
typedef struct { Str path, content; } RawFile;
typedef struct {
  Site site;
  RawTemplate *templates; size_t ntemplates;
  RawFile *files; size_t nfiles;
} Input;

// Parse cases.json (Way B). Returns number of cases; fills inputs[] (arena).
static int parse_cases(const uint8_t *data, size_t len, Arena *arena, Input **inputs_out) {
  J j = { data, data + len, arena };
  j_ws(&j);
  if (j.p >= j.end || *j.p != '[') return -1;
  j.p++;
  // count cases first via a quick scan is complex; grow dynamically
  size_t cap = 4, n = 0;
  Input *inputs = arena_alloc(arena, cap * sizeof(Input));
  j_ws(&j);
  if (j.p < j.end && *j.p == ']') { j.p++; *inputs_out = inputs; return 0; }
  for (;;) {
    j_ws(&j);
    if (j.p >= j.end || *j.p != '{') return -1;
    j.p++;
    Input in; in.ntemplates = 0; in.nfiles = 0; in.templates = NULL; in.files = NULL;
    in.site.title.p = NULL; in.site.title.len = 0;
    in.site.base_url.p = NULL; in.site.base_url.len = 0;
    in.site.per_page = 0; in.site.recent = 0;
    for (;;) {
      j_ws(&j);
      if (j.p < j.end && *j.p == '}') { j.p++; break; }
      Str key = j_string(&j);
      j_ws(&j);
      if (j.p < j.end && *j.p == ':') j.p++;
      j_ws(&j);
      if (str_eq(key, str_from("input"))) {
        if (j.p >= j.end || *j.p != '{') return -1;
        j.p++;
        for (;;) {
          j_ws(&j);
          if (j.p < j.end && *j.p == '}') { j.p++; break; }
          Str k2 = j_string(&j);
          j_ws(&j);
          if (j.p < j.end && *j.p == ':') j.p++;
          j_ws(&j);
          if (str_eq(k2, str_from("site"))) {
            if (j.p >= j.end || *j.p != '{') return -1;
            j.p++;
            for (;;) {
              j_ws(&j);
              if (j.p < j.end && *j.p == '}') { j.p++; break; }
              Str k3 = j_string(&j);
              j_ws(&j);
              if (j.p < j.end && *j.p == ':') j.p++;
              j_ws(&j);
              if (str_eq(k3, str_from("title"))) in.site.title = j_string(&j);
              else if (str_eq(k3, str_from("base_url"))) in.site.base_url = j_string(&j);
              else if (str_eq(k3, str_from("per_page"))) in.site.per_page = j_int(&j);
              else if (str_eq(k3, str_from("recent"))) in.site.recent = j_int(&j);
              else j_skip(&j);
              j_ws(&j);
              if (j.p < j.end && *j.p == ',') { j.p++; continue; }
              if (j.p < j.end && *j.p == '}') { j.p++; break; }
              return -1;
            }
          } else if (str_eq(k2, str_from("templates"))) {
            if (j.p >= j.end || *j.p != '{') return -1;
            j.p++;
            size_t tcap = 32, tn = 0;
            RawTemplate *ts = arena_alloc(arena, tcap * sizeof(RawTemplate));
            j_ws(&j);
            if (j.p < j.end && *j.p == '}') { j.p++; }
            else {
              for (;;) {
                j_ws(&j);
                Str name = j_string(&j);
                j_ws(&j);
                if (j.p < j.end && *j.p == ':') j.p++;
                j_ws(&j);
                Str src = j_string(&j);
                if (tn == tcap) return -1; // templates are few; arena grow omitted
                ts[tn].name = name; ts[tn].src = src; tn++;
                j_ws(&j);
                if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                if (j.p < j.end && *j.p == '}') { j.p++; break; }
                return -1;
              }
            }
            in.templates = ts; in.ntemplates = tn;
          } else if (str_eq(k2, str_from("files"))) {
            if (j.p >= j.end || *j.p != '[') return -1;
            j.p++;
            size_t fcap = 1024, fn = 0;
            RawFile *fs = arena_alloc(arena, fcap * sizeof(RawFile));
            j_ws(&j);
            if (j.p < j.end && *j.p == ']') { j.p++; }
            else {
              for (;;) {
                j_ws(&j);
                if (j.p >= j.end || *j.p != '{') return -1;
                j.p++;
                Str path = { 0, 0 }, content = { 0, 0 };
                for (;;) {
                  j_ws(&j);
                  if (j.p < j.end && *j.p == '}') { j.p++; break; }
                  Str k3 = j_string(&j);
                  j_ws(&j);
                  if (j.p < j.end && *j.p == ':') j.p++;
                  j_ws(&j);
                  if (str_eq(k3, str_from("path"))) path = j_string(&j);
                  else if (str_eq(k3, str_from("content"))) content = j_string(&j);
                  else j_skip(&j);
                  j_ws(&j);
                  if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                  if (j.p < j.end && *j.p == '}') { j.p++; break; }
                  return -1;
                }
                if (fn == fcap) return -1;
                fs[fn].path = path; fs[fn].content = content; fn++;
                j_ws(&j);
                if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                if (j.p < j.end && *j.p == ']') { j.p++; break; }
                return -1;
              }
            }
            in.files = fs; in.nfiles = fn;
          } else j_skip(&j);
          j_ws(&j);
          if (j.p < j.end && *j.p == ',') { j.p++; continue; }
          if (j.p < j.end && *j.p == '}') { j.p++; break; }
          return -1;
        }
      } else j_skip(&j);
      j_ws(&j);
      if (j.p < j.end && *j.p == ',') { j.p++; continue; }
      if (j.p < j.end && *j.p == '}') { j.p++; break; }
      return -1;
    }
    if (n == cap) return -1; // only 3 cases; arena grow omitted
    inputs[n++] = in;
    j_ws(&j);
    if (j.p < j.end && *j.p == ',') { j.p++; continue; }
    if (j.p < j.end && *j.p == ']') { j.p++; break; }
    return -1;
  }
  *inputs_out = inputs;
  return (int)n;
}

// ---------------------------------------------------------------- front matter
static const char *FM_KEYS[] = {"title","date","tags","draft","layout","slug","summary"};
static int fm_is_key(Str k) {
  for (int i = 0; i < 7; i++) if (str_eq(k, str_from(FM_KEYS[i]))) return 1;
  return 0;
}
static int days_in(int y, int m) {
  if (m == 2) return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}
// Format an error into the arena. Supports %s (Str) and %d.
typedef struct { Arena *a; Buf b; } Fmt;
static void fmt_init(Fmt *f, Arena *a) { f->a = a; buf_init(&f->b, a, 128); }
// valueOf for one key; returns 1 ok, 0 with *err set (arena Str).
typedef struct {
  int has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
  Str title, date, layout, slug, summary;
  Str *tags; size_t ntags;
  int draft;
} FMData;
static int fm_string_of(Str raw, Arena *a, Str *out, Str *err) {
  if (raw.len > 0 && raw.p[0] == '"') {
    if (raw.len < 2 || raw.p[raw.len - 1] != '"') { *err = str_from("a quoted string is not closed"); return 0; }
    uint8_t *o = arena_alloc(a, raw.len + 1);
    size_t n = 0;
    for (size_t i = 1; i + 1 < raw.len; i++) {
      uint8_t c = raw.p[i];
      if (c == '\\') {
        uint8_t nx = raw.p[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == raw.len - 1) { *err = str_from("a quoted string holds an escape other than \\\" or \\\\"); return 0; }
        o[n++] = nx; i++;
      } else if (c == '"') { *err = str_from("a quoted string holds a stray quote"); return 0; }
      else o[n++] = c;
    }
    out->p = o; out->len = n;
    return 1;
  }
  if (raw.len > 0 && raw.p[0] == '[') { *err = str_from("a bare string opens on ["); return 0; }
  *out = raw;
  return 1;
}
static int fm_value_of(Str key, Str raw, Arena *a, FMData *d, Str *err) {
  if (str_eq(key, str_from("title")) || str_eq(key, str_from("summary"))) {
    Str v;
    if (!fm_string_of(raw, a, &v, err)) {
      // prefix: key "title": <err>
      uint8_t *o = arena_alloc(a, 64 + err->len);
      size_t n = 0;
      const char *pre = str_eq(key, str_from("title")) ? "key \"title\": " : "key \"summary\": ";
      while (*pre) o[n++] = (uint8_t)*pre++;
      for (size_t i = 0; i < err->len; i++) o[n++] = err->p[i];
      err->p = o; err->len = n;
      return 0;
    }
    if (str_eq(key, str_from("title"))) d->title = v; else d->summary = v;
    return 1;
  }
  if (str_eq(key, str_from("date"))) {
    // YYYY-MM-DD, real day 1970-01-01..2099-12-31
    int ok = 0;
    if (raw.len == 10 && raw.p[4] == '-' && raw.p[7] == '-') {
      int dig = 1;
      for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (raw.p[i] < '0' || raw.p[i] > '9') dig = 0;
      }
      if (dig) {
        int y = (raw.p[0]-'0')*1000 + (raw.p[1]-'0')*100 + (raw.p[2]-'0')*10 + (raw.p[3]-'0');
        int mo = (raw.p[5]-'0')*10 + (raw.p[6]-'0');
        int dd = (raw.p[8]-'0')*10 + (raw.p[9]-'0');
        if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= days_in(y, mo)) ok = 1;
      }
    }
    if (!ok) { *err = str_from("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)"); return 0; }
    d->date = raw;
    return 1;
  }
  if (str_eq(key, str_from("tags"))) {
    if (raw.len < 2 || raw.p[0] != '[' || raw.p[raw.len - 1] != ']') { *err = str_from("key \"tags\": not a list [a, b]"); return 0; }
    Str inner = { raw.p + 1, raw.len - 2 };
    // trim check
    size_t ta = 0, b = inner.len;
    while (ta < b && (inner.p[ta] == ' ' || inner.p[ta] == '\t')) ta++;
    while (b > ta && (inner.p[b-1] == ' ' || inner.p[b-1] == '\t')) b--;
    if (ta == b) { d->tags = NULL; d->ntags = 0; return 1; }
    // split on ','
    size_t cap = 8, n = 0;
    Str *tags = arena_alloc(a, cap * sizeof(Str));
    size_t s = 0;
    for (size_t i = 0; i <= inner.len; i++) {
      if (i == inner.len || inner.p[i] == ',') {
        Str t = { inner.p + s, i - s };
        // trim spaces/tabs
        while (t.len > 0 && (t.p[0] == ' ' || t.p[0] == '\t')) { t.p++; t.len--; }
        while (t.len > 0 && (t.p[t.len-1] == ' ' || t.p[t.len-1] == '\t')) t.len--;
        if (t.len == 0) { *err = str_from("key \"tags\": an empty tag, or one holding \" [ or ]"); return 0; }
        for (size_t k = 0; k < t.len; k++) {
          if (t.p[k] == '"' || t.p[k] == '[' || t.p[k] == ']') { *err = str_from("key \"tags\": an empty tag, or one holding \" [ or ]"); return 0; }
        }
        if (n == cap) return 0; // tags are few
        tags[n++] = t;
        s = i + 1;
      }
    }
    d->tags = tags; d->ntags = n;
    return 1;
  }
  if (str_eq(key, str_from("draft"))) {
    if (str_eq(raw, str_from("true"))) { d->draft = 1; return 1; }
    if (str_eq(raw, str_from("false"))) { d->draft = 0; return 1; }
    *err = str_from("key \"draft\": true or false");
    return 0;
  }
  if (str_eq(key, str_from("layout"))) {
    int ok = raw.len > 0;
    for (size_t i = 0; i < raw.len; i++) {
      uint8_t c = raw.p[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
    }
    if (!ok) { *err = str_from("key \"layout\": not a template name"); return 0; }
    d->layout = raw;
    return 1;
  }
  if (str_eq(key, str_from("slug"))) {
    int ok = raw.len > 0 && raw.len <= 64;
    // ^[a-z0-9]+(?:-[a-z0-9]+)*$
    if (ok) {
      if (raw.p[0] == '-' || raw.p[raw.len-1] == '-') ok = 0;
      for (size_t i = 0; i < raw.len && ok; i++) {
        uint8_t c = raw.p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
        if (c == '-' && i + 1 < raw.len && raw.p[i+1] == '-') ok = 0;
      }
    }
    if (!ok) { *err = str_from("key \"slug\": not a slug"); return 0; }
    d->slug = raw;
    return 1;
  }
  *err = str_from("unknown key");
  return 0;
}
// Parse front matter of text (already \n-normalized). Returns 1 ok (fills
// d + body), 0 with *err (arena Str, the reason only).
static int parse_front_matter(Str text, Arena *a, FMData *d, Str *body, Str *err) {
  // first line must be exactly "---"
  const uint8_t *p = text.p, *end = text.p + text.len;
  const uint8_t *nl = p;
  while (nl < end && *nl != '\n') nl++;
  if ((size_t)(nl - p) != 3 || p[0] != '-' || p[1] != '-' || p[2] != '-') { *err = str_from("no front matter"); return 0; }
  // find the closing --- line
  const uint8_t *cur = nl < end ? nl + 1 : end;
  const uint8_t *close_start = NULL, *close_end = NULL;
  while (cur <= end) {
    const uint8_t *eol = cur;
    while (eol < end && *eol != '\n') eol++;
    if ((size_t)(eol - cur) == 3 && cur[0] == '-' && cur[1] == '-' && cur[2] == '-') { close_start = cur; close_end = eol; break; }
    if (eol == end) break;
    cur = eol + 1;
  }
  if (!close_start) { *err = str_from("the front matter is not closed"); return 0; }
  *d = (FMData){0};
  // fm lines: from after the first \n to close_start (excl trailing \n)
  const uint8_t *fm_start = nl < end ? nl + 1 : end;
  const uint8_t *fm_end = close_start;
  if (fm_end > fm_start && fm_end[-1] == '\n') fm_end--;
  // iterate lines with index (line number = idx+2)
  const uint8_t *lp = fm_start;
  int idx = 0;
  while (lp <= fm_end) {
    const uint8_t *le = lp;
    while (le < fm_end && *le != '\n') le++;
    Str line = { lp, (size_t)(le - lp) };
    // blank (spaces/tabs) or comment (# first)
    int blank = 1;
    for (size_t k = 0; k < line.len; k++) if (line.p[k] != ' ' && line.p[k] != '\t') { blank = 0; break; }
    if (!blank && line.len > 0 && line.p[0] == '#') blank = 2; // comment
    if (blank == 0) {
      // key: value — key [a-z_]+, then ':' then end or ' ' + value
      size_t c = 0;
      while (c < line.len && line.p[c] != ':') c++;
      if (c == line.len) goto not_kv;
      // key must be [a-z_]+
      int keyok = c > 0;
      for (size_t k = 0; k < c; k++) {
        uint8_t ch = line.p[k];
        if (!((ch >= 'a' && ch <= 'z') || ch == '_')) keyok = 0;
      }
      if (!keyok) goto not_kv;
      Str key = { line.p, c };
      Str raw;
      if (c == line.len - 1) { raw.p = line.p + line.len; raw.len = 0; }
      else if (line.p[c + 1] == ' ') { raw.p = line.p + c + 2; raw.len = line.len - (c + 2); }
      else goto not_kv;
      // trailing spaces/tabs dropped
      while (raw.len > 0 && (raw.p[raw.len-1] == ' ' || raw.p[raw.len-1] == '\t')) raw.len--;
      if (!fm_is_key(key)) {
        uint8_t *o = arena_alloc(a, 32 + key.len);
        size_t n = 0;
        const char *pre = "unknown key \"";
        while (*pre) o[n++] = (uint8_t)*pre++;
        for (size_t k = 0; k < key.len; k++) o[n++] = key.p[k];
        o[n++] = '"';
        err->p = o; err->len = n;
        return 0;
      }
      // duplicate?
      int dup = 0;
      if (str_eq(key, str_from("title"))) dup = d->has_title;
      else if (str_eq(key, str_from("date"))) dup = d->has_date;
      else if (str_eq(key, str_from("tags"))) dup = d->has_tags;
      else if (str_eq(key, str_from("draft"))) dup = d->has_draft;
      else if (str_eq(key, str_from("layout"))) dup = d->has_layout;
      else if (str_eq(key, str_from("slug"))) dup = d->has_slug;
      else if (str_eq(key, str_from("summary"))) dup = d->has_summary;
      if (dup) {
        uint8_t *o = arena_alloc(a, 32 + key.len);
        size_t n = 0;
        const char *pre = "key \"";
        while (*pre) o[n++] = (uint8_t)*pre++;
        for (size_t k = 0; k < key.len; k++) o[n++] = key.p[k];
        const char *suf = "\" is given twice";
        while (*suf) o[n++] = (uint8_t)*suf++;
        err->p = o; err->len = n;
        return 0;
      }
      if (raw.len == 0) {
        uint8_t *o = arena_alloc(a, 32 + key.len);
        size_t n = 0;
        const char *pre = "key \"";
        while (*pre) o[n++] = (uint8_t)*pre++;
        for (size_t k = 0; k < key.len; k++) o[n++] = key.p[k];
        const char *suf = "\" has no value";
        while (*suf) o[n++] = (uint8_t)*suf++;
        err->p = o; err->len = n;
        return 0;
      }
      Str verr;
      if (!fm_value_of(key, raw, a, d, &verr)) { *err = verr; return 0; }
      if (str_eq(key, str_from("title"))) d->has_title = 1;
      else if (str_eq(key, str_from("date"))) d->has_date = 1;
      else if (str_eq(key, str_from("tags"))) d->has_tags = 1;
      else if (str_eq(key, str_from("draft"))) d->has_draft = 1;
      else if (str_eq(key, str_from("layout"))) d->has_layout = 1;
      else if (str_eq(key, str_from("slug"))) d->has_slug = 1;
      else if (str_eq(key, str_from("summary"))) d->has_summary = 1;
    }
    if (le == fm_end) break;
    lp = le + 1;
    idx++;
    continue;
  not_kv: {
      uint8_t *o = arena_alloc(a, 48);
      size_t n = 0;
      const char *pre = "line ";
      while (*pre) o[n++] = (uint8_t)*pre++;
      // idx+2 as decimal
      int ln = idx + 2;
      char tmp[8]; int tn = 0;
      if (ln == 0) tmp[tn++] = '0';
      while (ln > 0) { tmp[tn++] = (char)('0' + ln % 10); ln /= 10; }
      while (tn > 0) o[n++] = (uint8_t)tmp[--tn];
      const char *suf = " is not \"key: value\"";
      while (*suf) o[n++] = (uint8_t)*suf++;
      err->p = o; err->len = n;
      return 0;
    }
  }
  if (!d->has_title) { *err = str_from("no title"); return 0; }
  body->p = close_end < end ? close_end + 1 : end;
  body->len = (size_t)(end - body->p);
  return 1;
}

// ---------------------------------------------------------------- values (template data model)
typedef enum { V_UNDEF, V_NULL, V_STR, V_NUM, V_BOOL, V_ARR, V_OBJ } VKind;
typedef struct Value Value;
typedef struct { Str name; Value *v; } Field;
struct Value {
  VKind kind;
  Str s;        // V_STR
  int64_t num;  // V_NUM
  int boolean;  // V_BOOL
  Value **items; size_t nitems; // V_ARR
  Field *fields; size_t nfields; // V_OBJ
};
static Value *v_new(Arena *a, VKind k) {
  Value *v = arena_alloc(a, sizeof(Value));
  v->kind = k; v->s.p = NULL; v->s.len = 0; v->num = 0; v->boolean = 0;
  v->items = NULL; v->nitems = 0; v->fields = NULL; v->nfields = 0;
  return v;
}
static Value *v_str(Arena *a, Str s) { Value *v = v_new(a, V_STR); v->s = s; return v; }
static Value *v_num(Arena *a, int64_t n) { Value *v = v_new(a, V_NUM); v->num = n; return v; }
static Value *v_bool(Arena *a, int b) { Value *v = v_new(a, V_BOOL); v->boolean = b; return v; }
static Value *v_null(Arena *a) { return v_new(a, V_NULL); }
static Value *v_undef(Arena *a) { return v_new(a, V_UNDEF); }
static Value *v_arr(Arena *a, Value **items, size_t n) { Value *v = v_new(a, V_ARR); v->items = items; v->nitems = n; return v; }
static Value *v_obj(Arena *a, Field *fields, size_t n) { Value *v = v_new(a, V_OBJ); v->fields = fields; v->nfields = n; return v; }
static Value *v_field(Value *obj, Str name) {
  if (!obj || obj->kind != V_OBJ) return NULL;
  for (size_t i = 0; i < obj->nfields; i++) if (str_eq(obj->fields[i].name, name)) return obj->fields[i].v;
  return NULL;
}

// ---------------------------------------------------------------- build model
typedef struct Source Source;
typedef struct Tag Tag;
typedef struct { int level; Str id, html; } TocEntry;
struct Source {
  Str path, text;
  FMData fm;
  Str body;
  int post;
  Str layout;
  Str slug, outDir, url;
  Str html;
  TocEntry *toc; int ntoc;
  Str excerpt;
  Source *newer, *older;
  Tag **tagRefs; int ntagRefs;
  Value *card;
};
struct Tag {
  Str name, slug, url;
  Source **posts; int nposts, posts_cap;
};
typedef struct {
  Str name, src;
  // compiled ops (see templates section)
  void *ops; int nops;
  int parse_err; Str err_msg;
} Template;
typedef struct Build {
  Arena *arena;
  Input *input;
  Str base;
  Template *templates; int ntemplates;
  HMap template_map;
  Str *report; int nreport, report_cap;
  Source *sources; int nsources, sources_cap;
  RawFile *statics; int nstatics;
  HMap by_source;
  StrSet static_paths;
  StrSet broken;
  Source **posts; int nposts;
  Tag *tags; int ntags, tags_cap;
  Str *out_keys; Str *out_vals; int nout, out_cap;
  Value *site_v, *tags_v, *recent_v; // built once per case
} Build;
static void build_report(Build *b, Str line) {
  if (b->nreport == b->report_cap) {
    b->report_cap = b->report_cap ? b->report_cap * 2 : 32;
    Str *nr = arena_alloc(b->arena, b->report_cap * sizeof(Str));
    for (int i = 0; i < b->nreport; i++) nr[i] = b->report[i];
    b->report = nr;
  }
  b->report[b->nreport++] = line;
}
static void build_out(Build *b, Str k, Str v) {
  if (b->nout == b->out_cap) {
    b->out_cap = b->out_cap ? b->out_cap * 2 : 64;
    Str *nk = arena_alloc(b->arena, b->out_cap * sizeof(Str));
    Str *nv = arena_alloc(b->arena, b->out_cap * sizeof(Str));
    for (int i = 0; i < b->nout; i++) { nk[i] = b->out_keys[i]; nv[i] = b->out_vals[i]; }
    b->out_keys = nk; b->out_vals = nv;
  }
  b->out_keys[b->nout] = k; b->out_vals[b->nout] = v; b->nout++;
}

// link resolution (env.link)
typedef struct { Build *b; Str dir; Str src_path; } LinkCtx;
static Str resolve_path(Arena *a, Str dir, Str rel) {
  // fast path: no dot segment, no '//', not starting/ending with '/'
  int has_dot = 0, has_dslash = 0;
  for (size_t i = 0; i < rel.len; i++) {
    if (rel.p[i] == '.') has_dot = 1;
    if (rel.p[i] == '/' && i + 1 < rel.len && rel.p[i+1] == '/') has_dslash = 1;
  }
  if (!has_dot && !has_dslash && rel.len > 0 && rel.p[0] != '/' && rel.p[rel.len-1] != '/') {
    if (dir.len == 0) return rel;
    uint8_t *o = arena_alloc(a, dir.len + 1 + rel.len);
    size_t n = 0;
    for (size_t i = 0; i < dir.len; i++) o[n++] = dir.p[i];
    o[n++] = '/';
    for (size_t i = 0; i < rel.len; i++) o[n++] = rel.p[i];
    Str r = { o, n };
    return r;
  }
  // slow path: split on '/'
  // parts: dir segments (arena array of Str) then apply rel segments
  Str *parts = arena_alloc(a, (dir.len + rel.len + 2) * sizeof(Str));
  int np = 0;
  if (dir.len > 0) {
    size_t s = 0;
    for (size_t i = 0; i <= dir.len; i++) {
      if (i == dir.len || dir.p[i] == '/') { parts[np].p = dir.p + s; parts[np].len = i - s; np++; s = i + 1; }
    }
  }
  size_t s = 0;
  for (size_t i = 0; i <= rel.len; i++) {
    if (i == rel.len || rel.p[i] == '/') {
      Str seg = { rel.p + s, i - s };
      s = i + 1;
      if (seg.len == 0) continue;
      if (seg.len == 1 && seg.p[0] == '.') continue;
      if (seg.len == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
        if (np == 0) { Str null_r = { 0, 0 }; return null_r; } // caller checks via a flag: use len==0 && p==NULL
        np--;
        continue;
      }
      parts[np++] = seg;
    }
  }
  // join with '/'
  size_t total = 0;
  for (int i = 0; i < np; i++) total += parts[i].len + 1;
  if (total == 0) { Str r = { (const uint8_t *)"", 0 }; return r; }
  uint8_t *o = arena_alloc(a, total);
  size_t n = 0;
  for (int i = 0; i < np; i++) {
    if (i > 0) o[n++] = '/';
    for (size_t k = 0; k < parts[i].len; k++) o[n++] = parts[i].p[k];
  }
  Str r = { o, n };
  return r;
}
static int has_scheme(Str t) {
  if (t.len < 2) return 0;
  uint8_t c = t.p[0];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
  size_t i = 1;
  while (i < t.len) {
    uint8_t ch = t.p[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.') { i++; continue; }
    break;
  }
  return i < t.len && t.p[i] == ':';
}
static Str link_resolve(LinkCtx *ctx, Str target, int *broken_out) {
  Build *b = ctx->b;
  Arena *a = b->arena;
  if (has_scheme(target) || (target.len > 0 && target.p[0] == '/') || (target.len > 0 && target.p[0] == '#')) { *broken_out = 0; return target; }
  // split off #fragment
  size_t hash = 0;
  int has_hash = 0;
  for (size_t i = 0; i < target.len; i++) if (target.p[i] == '#') { hash = i; has_hash = 1; break; }
  Str rel = has_hash ? (Str){ target.p, hash } : target;
  Str frag = has_hash ? (Str){ target.p + hash, target.len - hash } : (Str){ 0, 0 };
  Str path = { 0, 0 };
  int have_path = 0;
  if (rel.len > 0) {
    path = resolve_path(a, ctx->dir, rel);
    have_path = !(path.p == NULL && path.len == 0);
    if (!have_path && path.p == NULL) have_path = 0;
    // resolve_path returns p==NULL only on climb-out (null); empty path "" has p!=NULL
    if (path.p == NULL) have_path = 0;
    else have_path = 1;
  }
  if (have_path) {
    Source *s = hmap_get(&b->by_source, path);
    if (s) {
      // url + frag
      uint8_t *o = arena_alloc(a, s->url.len + frag.len);
      size_t n = 0;
      for (size_t i = 0; i < s->url.len; i++) o[n++] = s->url.p[i];
      for (size_t i = 0; i < frag.len; i++) o[n++] = frag.p[i];
      Str r = { o, n };
      *broken_out = 0;
      return r;
    }
    if (strset_has(&b->static_paths, path)) {
      uint8_t *o = arena_alloc(a, b->base.len + 1 + path.len + frag.len);
      size_t n = 0;
      for (size_t i = 0; i < b->base.len; i++) o[n++] = b->base.p[i];
      o[n++] = '/';
      for (size_t i = 0; i < path.len; i++) o[n++] = path.p[i];
      for (size_t i = 0; i < frag.len; i++) o[n++] = frag.p[i];
      Str r = { o, n };
      *broken_out = 0;
      return r;
    }
  }
  *broken_out = 1;
  return target;
}
// Report a broken link once per (source, target), deduped by message.
// Called by linkFn for EVERY unresolved occurrence -- including memo hits
// (the memo caches the resolution, not the report: two sources in one
// directory linking the same broken target each get their own line).
static void report_broken_link(LinkCtx *ctx, Str target) {
  Build *b = ctx->b;
  Arena *a = b->arena;
  const char *pre = "broken-link ";
  const char *mid = " -> ";
  size_t total = 12 + ctx->src_path.len + 4 + target.len;
  uint8_t *o = arena_alloc(a, total + 1);
  size_t n = 0;
  for (size_t i = 0; pre[i]; i++) o[n++] = (uint8_t)pre[i];
  for (size_t i = 0; i < ctx->src_path.len; i++) o[n++] = ctx->src_path.p[i];
  for (size_t i = 0; mid[i]; i++) o[n++] = (uint8_t)mid[i];
  for (size_t i = 0; i < target.len; i++) o[n++] = target.p[i];
  Str msg = { o, n };
  if (!strset_has(&b->broken, msg)) strset_add(&b->broken, msg);
}

// ---------------------------------------------------------------- markdown (md_core.c)
// The markdown renderer lives in md_core.c — a verbatim copy of the WASM
// prototype's c/md.c (byte-exact port of solve.js renderMarkdown +
// inlineTo; 0 mismatches on 1,024 practice bodies + 113 edge cases + 4,000
// fuzz bodies in the prototype's differential, ASan/UBSan clean). It is kept
// as its own translation unit so it stays pristine. Its only boundary points
// are the two imports below, implemented here as direct in-engine calls —
// NO per-link/per-slug callbacks across any boundary, so the +18.3ms import
// tax measured in the WASM prototype does not exist on this path.
// NOTE: md_core has fixed static buffers (IN 8 MiB / OUT 4 MiB / JOIN 2 MiB
// per body). Practice bodies are ~2.5 KB (max measured in the differential
// run, recorded in versions.md); a body over the caps cannot occur for
// practice-shaped data. render_markdown guards hlen<0 by emitting empty html.
#include "md_core.h"
typedef struct { TocEntry *items; int n, cap; Arena *arena; Str excerpt; int has_excerpt; } MdOut;
static void md_toc_push(MdOut *res, int level, Str id, Str html) {
  if (res->n == res->cap) {
    res->cap = res->cap ? res->cap * 2 : 8;
    TocEntry *ni = arena_alloc(res->arena, (size_t)res->cap * sizeof(TocEntry));
    for (int i = 0; i < res->n; i++) ni[i] = res->items[i];
    res->items = ni;
  }
  res->items[res->n].level = level;
  res->items[res->n].id = id;
  res->items[res->n].html = html;
  res->n++;
}


// ---- link-resolution memo: (dir, target) -> resolved url ----
// link_resolve costs ~1us per call (resolve_path alloc + hmap + result alloc);
// the same (dir, target) pairs repeat across bodies (5,747 linkFn calls/run).
// 128-slot direct-mapped cache; entries hold arena Strs valid for one case
// only, so the cache is cleared at the start of each build_case.
typedef struct { int valid; Str dir; Str target; Str url; int broken; } LinkMemo;
static LinkMemo g_link_memo[128];
static void link_memo_clear(void) {
  for (int i = 0; i < 128; i++) g_link_memo[i].valid = 0;
}
static uint64_t link_memo_hash(Str dir, Str target) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < dir.len; i++) { h ^= dir.p[i]; h *= 1099511628211ULL; }
  h ^= 0xFF; h *= 1099511628211ULL;
  for (size_t i = 0; i < target.len; i++) { h ^= target.p[i]; h *= 1099511628211ULL; }
  return h;
}

static LinkCtx *g_md_ctx;
static Arena *g_md_arena;
static uint32_t md_u32(const uint8_t *m, int off) { uint32_t v; __builtin_memcpy(&v, m + off, 4); return v; }
static void md_store_u32(uint8_t *m, uint32_t v) { __builtin_memcpy(m, &v, 4); }

// md_core link import: resolve in-engine. Return 1 + write u32 len + URL
// bytes to SCRATCH when resolved; 0 when unresolved (core then emits the raw
// target, exactly like the JS reference). link_resolve returns the target
// itself for scheme/absolute/fragment/broken, so byte-equality is the test.
int linkFn(const uint8_t *p, int len) {
  Str target = { p, (size_t)len };
  uint64_t h = link_memo_hash(g_md_ctx->dir, target);
  LinkMemo *m = &g_link_memo[h & 127];
  Str url;
  int broken;
  if (m->valid && str_eq(m->dir, g_md_ctx->dir) && str_eq(m->target, target)) {
    url = m->url;
    broken = m->broken;
  } else {
    url = link_resolve(g_md_ctx, target, &broken);
    m->valid = 1; m->dir = g_md_ctx->dir; m->target = target; m->url = url; m->broken = broken;
  }
  if (broken) { report_broken_link(g_md_ctx, target); return 0; }
  uint8_t *sc = scratch_ptr();
  md_store_u32(sc, (uint32_t)url.len);
  __builtin_memcpy(sc + 4, url.p, url.len);
  return 1;
}
// md_core slug import (non-ASCII headings only): NFKD slug via slugtab.
int slugFn(const uint8_t *p, int len) {
  Str text = { p, (size_t)len };
  Str s = slugify(g_md_arena, text);
  uint8_t *sc = scratch2_ptr();
  md_store_u32(sc, (uint32_t)s.len);
  __builtin_memcpy(sc + 4, s.p, s.len);
  return (int)s.len;
}

// Same signature as the old hand-written renderer: html to Buf out,
// toc + excerpt to res. META layout: [htmlLen][excerptOff][excerptLen]
// [tocCount] then tocCount 20-byte records
// [level][idOff][idLen][htmlOff][htmlLen], offsets OUT-relative.
static void render_markdown(Str src, LinkCtx *ctx, Buf *out, MdOut *res) {
  g_md_ctx = ctx;
  g_md_arena = out->arena;
  uint8_t *in = in_ptr();
  __builtin_memcpy(in, src.p, src.len);
  int hlen = render_md((int)src.len);
  if (hlen < 0) hlen = 0;
  uint8_t *ob = out_ptr();
  buf_put(out, ob, (size_t)hlen);
  const uint8_t *meta = meta_ptr();
  uint32_t ex_off = md_u32(meta, 4);
  uint32_t ex_len = md_u32(meta, 8);
  uint32_t tn = md_u32(meta, 12);
  if (ex_len > 0) {
    uint8_t *ep = arena_alloc(res->arena, ex_len);
    __builtin_memcpy(ep, ob + ex_off, ex_len);
    res->excerpt.p = ep; res->excerpt.len = ex_len; res->has_excerpt = 1;
  }
  for (uint32_t i = 0; i < tn; i++) {
    const uint8_t *rec = meta + 16 + (size_t)i * 20;
    int level = (int)md_u32(rec, 0);
    uint32_t id_off = md_u32(rec, 4), id_len = md_u32(rec, 8);
    uint32_t h_off = md_u32(rec, 12), h_len = md_u32(rec, 16);
    uint8_t *ip = arena_alloc(res->arena, id_len ? id_len : 1);
    __builtin_memcpy(ip, ob + id_off, id_len);
    uint8_t *hp = arena_alloc(res->arena, h_len ? h_len : 1);
    __builtin_memcpy(hp, ob + h_off, h_len);
    md_toc_push(res, level, (Str){ ip, id_len }, (Str){ hp, h_len });
  }
}

// ---------------------------------------------------------------- templates
enum { OP_TEXT, OP_VAR, OP_RAW, OP_IF, OP_ELSE, OP_ENDIF, OP_FOR, OP_ENDFOR, OP_INCLUDE };
typedef struct {
  uint8_t kind;
  Str text;             // OP_TEXT
  Str *path; int npath; // OP_VAR/RAW/IF/FOR
  int not;              // OP_IF
  Str varname;          // OP_FOR
  Str incname;          // OP_INCLUDE
  int jump;             // OP_IF -> else/endif; OP_ELSE -> endif; OP_FOR -> endfor
} Op;
typedef struct {
  Op *ops; int nops, cap;
  int parse_err; Str err_msg;
} Tmpl;
static void tmpl_push(Tmpl *t, Arena *a, Op op) {
  if (t->nops == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 16;
    Op *no = arena_alloc(a, (size_t)t->cap * sizeof(Op));
    for (int i = 0; i < t->nops; i++) no[i] = t->ops[i];
    t->ops = no;
  }
  t->ops[t->nops++] = op;
}
// JSON.stringify of a string (for error messages), arena Str.
static Str json_stringify_str(Arena *a, Str s) {
  Buf b; buf_init(&b, a, s.len + 8);
  json_emit_str(&b, s);
  Str r = { b.buf, b.len };
  return r;
}
static Str tmpl_err(Arena *a, Str name, const char *msg) {
  Buf b; buf_init(&b, a, 128);
  buf_puts(&b, "template \"");
  buf_put_str(&b, name);
  buf_puts(&b, "\": ");
  buf_puts(&b, msg);
  Str r = { b.buf, b.len };
  return r;
}
static int path_valid(Str t) {
  // /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/
  if (t.len == 0) return 0;
  uint8_t c = t.p[0];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
  for (size_t i = 1; i < t.len; i++) {
    uint8_t ch = t.p[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.') continue;
    return 0;
  }
  // no leading/trailing/consecutive dots; segments must start with letter/_
  if (t.p[t.len-1] == '.') return 0;
  for (size_t i = 0; i < t.len; i++) {
    if (t.p[i] == '.') {
      if (i + 1 >= t.len) return 0;
      uint8_t nx = t.p[i+1];
      if (!((nx >= 'A' && nx <= 'Z') || (nx >= 'a' && nx <= 'z') || nx == '_')) return 0;
    }
  }
  return 1;
}
static Str *path_split(Arena *a, Str t, int *n_out) {
  int n = 1;
  for (size_t i = 0; i < t.len; i++) if (t.p[i] == '.') n++;
  Str *parts = arena_alloc(a, (size_t)n * sizeof(Str));
  int k = 0;
  size_t s = 0;
  for (size_t i = 0; i <= t.len; i++) {
    if (i == t.len || t.p[i] == '.') { parts[k].p = t.p + s; parts[k].len = i - s; k++; s = i + 1; }
  }
  *n_out = n;
  return parts;
}
// Parse a template source into flat ops. Returns 1 ok, 0 with err set.
static int parse_template(Str name, Str src, Arena *a, Tmpl *t) {
  t->ops = NULL; t->nops = 0; t->cap = 0; t->parse_err = 0;
  // tokenize + parse in one pass with a stack of open constructs
  typedef struct { int kind; int op_idx; int else_idx; } Frame; // kind 0=for, 1=if
  Frame *stack = arena_alloc(a, 64 * sizeof(Frame));
  int nstack = 0;
  Buf text; buf_init(&text, a, 256);
  size_t i = 0;
  const uint8_t *s = src.p;
  size_t N = src.len;
  while (i < N) {
    // find next '{'
    size_t open = i;
    int found = 0;
    while (open < N) {
      if (s[open] == '{') { found = 1; break; }
      open++;
    }
    if (!found || open == N - 1) {
      buf_put(&text, s + i, N - i);
      break;
    }
    uint8_t next = s[open + 1];
    if (next != '{' && next != '%') {
      buf_put(&text, s + i, (open + 1) - i);
      i = open + 1;
      continue;
    }
    buf_put(&text, s + i, open - i);
    if (next == '{') {
      int raw = (open + 2 < N && s[open + 2] == '{');
      const char *close_str = raw ? "}}}" : "}}";
      size_t clen = raw ? 3 : 2;
      // find close
      size_t close = N + 1;
      for (size_t k = open + clen; k + clen <= N; k++) {
        if (__builtin_memcmp(s + k, close_str, clen) == 0) { close = k; break; }
      }
      if (close == N + 1) {
        t->parse_err = 1;
        t->err_msg = tmpl_err(a, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        return 0;
      }
      if (text.len > 0) { Op op; op.kind = OP_TEXT; op.text.p = text.buf; op.text.len = text.len; op.path = NULL; op.npath = 0; tmpl_push(t, a, op); buf_init(&text, a, 256); /* fresh block: the op keeps the old one */ }
      Str inner = { s + open + clen, close - (open + clen) };
      // trim spaces/tabs
      while (inner.len > 0 && (inner.p[0] == ' ' || inner.p[0] == '\t')) { inner.p++; inner.len--; }
      while (inner.len > 0 && (inner.p[inner.len-1] == ' ' || inner.p[inner.len-1] == '\t')) inner.len--;
      if (!path_valid(inner)) {
        Str js = json_stringify_str(a, inner);
        Buf eb; buf_init(&eb, a, 128);
        buf_puts(&eb, "template \"");
        buf_put_str(&eb, name);
        buf_puts(&eb, "\": ");
        buf_put_str(&eb, js);
        buf_puts(&eb, " is not a path");
        t->parse_err = 1;
        t->err_msg.p = eb.buf; t->err_msg.len = eb.len;
        return 0;
      }
      Op op; op.kind = raw ? OP_RAW : OP_VAR;
      op.path = path_split(a, inner, &op.npath);
      op.text.p = 0; op.text.len = 0;
      tmpl_push(t, a, op);
      i = close + clen;
      continue;
    }
    // {% %}
    size_t close = N + 1;
    for (size_t k = open + 2; k + 1 < N; k++) {
      if (s[k] == '%' && s[k+1] == '}') { close = k; break; }
    }
    if (close == N + 1) {
      t->parse_err = 1;
      t->err_msg = tmpl_err(a, name, "a {% is not closed");
      return 0;
    }
    size_t end = close + 2;
    // standalone: only spaces/tabs beside the tag on its line
    size_t lineStart = open;
    while (lineStart > 0 && s[lineStart - 1] != '\n') lineStart--;
    size_t nl = end;
    while (nl < N && s[nl] != '\n') nl++;
    size_t lineEnd = nl;
    int pre_ok = 1;
    for (size_t k = lineStart; k < open; k++) if (s[k] != ' ' && s[k] != '\t') pre_ok = 0;
    int post_ok = 1;
    for (size_t k = end; k < lineEnd; k++) if (s[k] != ' ' && s[k] != '\t') post_ok = 0;
    int standalone = pre_ok && post_ok;
    if (standalone) {
      // remove (open - lineStart) chars from the accumulated text
      size_t cut = open - lineStart;
      if (cut > text.len) text.len = 0; else text.len -= cut;
    }
    if (text.len > 0) { Op op; op.kind = OP_TEXT; op.text.p = text.buf; op.text.len = text.len; op.path = NULL; op.npath = 0; tmpl_push(t, a, op); buf_init(&text, a, 256); /* fresh block: the op keeps the old one */ }
    Str tag = { s + open + 2, close - (open + 2) };
    while (tag.len > 0 && (tag.p[0] == ' ' || tag.p[0] == '\t')) { tag.p++; tag.len--; }
    while (tag.len > 0 && (tag.p[tag.len-1] == ' ' || tag.p[tag.len-1] == '\t')) tag.len--;
    // dispatch the tag
    // for X in PATH
    if (tag.len > 4 && __builtin_memcmp(tag.p, "for ", 4) == 0) {
      Str rest = { tag.p + 4, tag.len - 4 };
      // for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)
      size_t sp = 0;
      while (sp < rest.len && rest.p[sp] != ' ') sp++;
      Str varname = { rest.p, sp };
      // varname must be [A-Za-z_][A-Za-z0-9_]*
      int vok = varname.len > 0;
      if (vok) {
        uint8_t c = varname.p[0];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) vok = 0;
        for (size_t k = 1; k < varname.len && vok; k++) {
          uint8_t ch = varname.p[k];
          if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) vok = 0;
        }
      }
      Str pth = { 0, 0 };
      int pok = 0;
      if (vok && sp + 4 <= rest.len && __builtin_memcmp(rest.p + sp, " in ", 4) == 0) {
        pth.p = rest.p + sp + 4; pth.len = rest.len - (sp + 4);
        // \S+ : no whitespace, whole rest
        pok = pth.len > 0;
        for (size_t k = 0; k < pth.len && pok; k++) {
          uint8_t ch = pth.p[k];
          if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') pok = 0;
        }
        if (pok && !path_valid(pth)) pok = 0;
      }
      if (vok && pok) {
        Op op; op.kind = OP_FOR; op.varname = varname; op.path = path_split(a, pth, &op.npath); op.jump = -1;
        tmpl_push(t, a, op);
        if (nstack < 64) { stack[nstack].kind = 0; stack[nstack].op_idx = t->nops - 1; stack[nstack].else_idx = -1; nstack++; }
        i = standalone ? (nl == N ? N : nl + 1) : end;
        continue;
      }
      // fall through to "not a tag"
    } else if (tag.len > 3 && __builtin_memcmp(tag.p, "if ", 3) == 0) {
      Str rest = { tag.p + 3, tag.len - 3 };
      int not = 0;
      if (rest.len > 4 && __builtin_memcmp(rest.p, "not ", 4) == 0) { not = 1; rest.p += 4; rest.len -= 4; }
      if (rest.len > 0 && path_valid(rest)) {
        Op op; op.kind = OP_IF; op.not = not; op.path = path_split(a, rest, &op.npath); op.jump = -1;
        tmpl_push(t, a, op);
        if (nstack < 64) { stack[nstack].kind = 1; stack[nstack].op_idx = t->nops - 1; stack[nstack].else_idx = -1; nstack++; }
        i = standalone ? (nl == N ? N : nl + 1) : end;
        continue;
      }
      // fall through
    } else if (str_eq(tag, str_from("else"))) {
      if (nstack == 0 || stack[nstack-1].kind != 1 || stack[nstack-1].else_idx != -1) {
        t->parse_err = 1;
        t->err_msg = tmpl_err(a, name, "an else outside an if");
        return 0;
      }
      Op op; op.kind = OP_ELSE; op.jump = -1;
      tmpl_push(t, a, op);
      int if_idx = stack[nstack-1].op_idx;
      t->ops[if_idx].jump = t->nops - 1; // if -> else
      stack[nstack-1].else_idx = t->nops - 1;
      i = standalone ? (nl == N ? N : nl + 1) : end;
      continue;
    } else if (str_eq(tag, str_from("endfor")) || str_eq(tag, str_from("endif"))) {
      int want = str_eq(tag, str_from("endfor")) ? 0 : 1;
      if (nstack == 0 || stack[nstack-1].kind != want) {
        t->parse_err = 1;
        char msg[64];
        if (want == 0) __builtin_memcpy(msg, "an endfor with no open for", 27);
        else __builtin_memcpy(msg, "an endif with no open if", 25);
        msg[want == 0 ? 26 : 24] = 0;
        t->err_msg = tmpl_err(a, name, msg);
        return 0;
      }
      Op op; op.kind = want == 0 ? OP_ENDFOR : OP_ENDIF; op.jump = -1;
      tmpl_push(t, a, op);
      int open_idx = stack[nstack-1].op_idx;
      int else_idx = stack[nstack-1].else_idx;
      if (want == 0) t->ops[open_idx].jump = t->nops - 1; // for -> endfor
      else {
        if (else_idx != -1) t->ops[else_idx].jump = t->nops - 1; // else -> endif
        else t->ops[open_idx].jump = t->nops - 1; // if -> endif
      }
      nstack--;
      i = standalone ? (nl == N ? N : nl + 1) : end;
      continue;
    } else if (tag.len > 8 && __builtin_memcmp(tag.p, "include ", 8) == 0) {
      Str rest = { tag.p + 8, tag.len - 8 };
      // include "([a-z0-9-]+)"
      if (rest.len >= 2 && rest.p[0] == '"' && rest.p[rest.len-1] == '"') {
        Str nm = { rest.p + 1, rest.len - 2 };
        int ok = nm.len > 0;
        for (size_t k = 0; k < nm.len && ok; k++) {
          uint8_t ch = nm.p[k];
          if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) ok = 0;
        }
        if (ok) {
          Op op; op.kind = OP_INCLUDE; op.incname = nm;
          tmpl_push(t, a, op);
          i = standalone ? (nl == N ? N : nl + 1) : end;
          continue;
        }
      }
      // fall through
    }
    // not a tag
    {
      Buf eb; buf_init(&eb, a, 128);
      buf_puts(&eb, "template \"");
      buf_put_str(&eb, name);
      buf_puts(&eb, "\": {% ");
      buf_put_str(&eb, tag);
      buf_puts(&eb, " %} is not a tag");
      t->parse_err = 1;
      t->err_msg.p = eb.buf; t->err_msg.len = eb.len;
      return 0;
    }
  }
  if (text.len > 0) { Op op; op.kind = OP_TEXT; op.text.p = text.buf; op.text.len = text.len; op.path = NULL; op.npath = 0; tmpl_push(t, a, op); }
  if (nstack > 0) {
    t->parse_err = 1;
    t->err_msg = tmpl_err(a, name, stack[nstack-1].kind == 0 ? "a for is not closed" : "a if is not closed");
    return 0;
  }
  return 1;
}

// ---- include-render memo (v7.2) ----
// The layouts step re-renders the same partials thousands of times: every
// post's card appears once on an index page and once per tag page (~5x), and
// every tag-list inside it too. Memoizing an include render whose output
// depends only on case-globals (site/tags/recent) plus the values of the
// loop variables bound at the include point cuts ~2/3 of the layouts work.
//
// Correctness: an include of template U at a render point with bound names
// B (the loop variables on the scope chain) is cacheable iff
//   free(U, B) subset of {site, tags, recent}
// where free(U, B) = names U reads (transitively through includes) that are
// neither in B nor bound by U's own for-loops at the read point, and
//   loop not in outer(U, B)
// (the loop object is mutated per iteration, so its pointer is not a key).
// outer(U, B) = names in B that U reads where the innermost binding is B
// (not shadowed by U's own for-loops). The memo key is (U, the Value*
// pointers of outer(U, B)) — those Values are created once per case, so the
// pointers are stable within a case. Tables are cleared per build_case.
//
// Name interning: template variable names are mapped to ids 0..63 for
// bitmasks. More than 64 distinct names -> analysis reports uncacheable
// (safe fallback); the memo never caches then.

#define MEMO_MAX_NAMES 64
static Str g_memo_names[MEMO_MAX_NAMES];
static int g_memo_nnames = 0;
static int memo_name_id(Str name) {
  for (int i = 0; i < g_memo_nnames; i++) if (str_eq(g_memo_names[i], name)) return i;
  if (g_memo_nnames >= MEMO_MAX_NAMES) return -1;
  g_memo_names[g_memo_nnames] = name;
  return g_memo_nnames++;
}
static int memo_id_of_cstr(const char *s) {
  Str name = { (const uint8_t *)s, __builtin_strlen(s) };
  return memo_name_id(name);
}

typedef struct { uint64_t free_mask; uint64_t outer_mask; int bad; } TmplAnalysis;

// Walk a template's ops (linear scan; for/endfor nest properly) collecting
// free reads (neither in outer_bound nor bound by this template's own fors
// at the read point) and outer reads (in outer_bound and not shadowed by
// this template's own fors). Recurses into includes with
// outer_bound' = outer_bound | own bindings at the include point.
// depth caps the include recursion at 8, matching the renderer.
static TmplAnalysis memo_analyze(Tmpl *tm, uint64_t outer_bound, int depth, HMap *tmap) {
  TmplAnalysis res; res.free_mask = 0; res.outer_mask = 0; res.bad = 0;
  if (!tm || depth > 8) { res.bad = 1; return res; }
  uint64_t own_stack[16];
  int own_sp = 0;
  uint64_t own_mask = 0;
  for (int i = 0; i < tm->nops; i++) {
    Op *op = &tm->ops[i];
    int id;
    switch (op->kind) {
      case OP_VAR:
      case OP_RAW:
      case OP_IF:
      case OP_FOR: {
        if (op->npath < 1) break;
        id = memo_name_id(op->path[0]);
        if (id < 0) { res.bad = 1; return res; }
        uint64_t bit = 1ULL << id;
        if (!(own_mask & bit)) {
          if (outer_bound & bit) res.outer_mask |= bit;
          else res.free_mask |= bit;
        }
        if (op->kind == OP_FOR) {
          int vid = memo_name_id(op->varname);
          int lid = memo_id_of_cstr("loop");
          if (vid < 0 || lid < 0) { res.bad = 1; return res; }
          if (own_sp >= 16) { res.bad = 1; return res; }
          own_stack[own_sp++] = own_mask;
          own_mask |= (1ULL << vid) | (1ULL << lid);
        }
        break;
      }
      case OP_ENDFOR: {
        if (own_sp > 0) own_mask = own_stack[--own_sp];
        break;
      }
      case OP_INCLUDE: {
        Template *t = hmap_get(tmap, op->incname);
        if (t && !t->parse_err) {
          Tmpl *sub = (Tmpl *)t->ops;
          TmplAnalysis sub_res = memo_analyze(sub, outer_bound | own_mask, depth + 1, tmap);
          if (sub_res.bad) { res.bad = 1; return res; }
          res.free_mask |= sub_res.free_mask;
          // only deps in THIS level's outer_bound are inputs; deps bound by
          // this template's own for-loops are derived inside the render.
          res.outer_mask |= (sub_res.outer_mask & outer_bound);
        }
        break;
      }
      default: break;
    }
  }
  return res;
}

// Analysis cache: keyed by (Tmpl*, outer_bound mask). Cleared per build_case
// (Tmpl pointers are per-case arena addresses and must not outlive a case).
#define MEMO_ANALYSIS_SLOTS 256
typedef struct { int valid; Tmpl *tm; uint64_t bound; TmplAnalysis res; } MemoAnalysisSlot;
static MemoAnalysisSlot g_memo_analysis[MEMO_ANALYSIS_SLOTS];
static uint32_t g_memo_gen = 0;
static TmplAnalysis *memo_analysis_for(Tmpl *tm, uint64_t bound, HMap *tmap) {
  uint64_t h = ((uint64_t)(uintptr_t)tm * 1099511628211ULL) ^ (bound * 1469598103934665603ULL);
  size_t slot = (size_t)(h & (MEMO_ANALYSIS_SLOTS - 1));
  for (int probe = 0; probe < MEMO_ANALYSIS_SLOTS; probe++) {
    MemoAnalysisSlot *s = &g_memo_analysis[(slot + probe) & (MEMO_ANALYSIS_SLOTS - 1)];
    if (!s->valid) {
      s->valid = 1; s->tm = tm; s->bound = bound;
      s->res = memo_analyze(tm, bound, 0, tmap);
      return &s->res;
    }
    if (s->tm == tm && s->bound == bound) return &s->res;
  }
  return NULL; // table full: do not cache this include
}

// Render memo: keyed by (Template*, outer-dep Value* pointers, up to 4).
// open addressing, 16384 slots, generation-gated per build_case.
#define MEMO_SLOTS 16384
typedef struct { uint32_t gen; Template *tm; int ndep; Value *dep[4]; Str html; } MemoSlot;
static MemoSlot g_memo[MEMO_SLOTS];
static uint64_t g_memo_hits = 0, g_memo_miss = 0, g_memo_nocache = 0;
static void memo_tables_clear(void) {
  g_memo_gen++;
  for (int i = 0; i < MEMO_ANALYSIS_SLOTS; i++) g_memo_analysis[i].valid = 0;
  // g_memo entries are generation-gated; no memset needed.
  // the name-interning table holds Strs into the case arena: reset it too.
  g_memo_nnames = 0;
}

// ---------------------------------------------------------------- template render
typedef struct Scope { Str *names; Value **vals; int n; struct Scope *parent; } Scope;
typedef struct { Build *b; } TmplCtx;
static int scope_has(Scope *s, Str name) {
  for (int i = s->n - 1; i >= 0; i--) if (str_eq(s->names[i], name)) return 1;
  return 0;
}
static Value *scope_get(Scope *s, Str name) {
  for (int i = s->n - 1; i >= 0; i--) if (str_eq(s->names[i], name)) return s->vals[i];
  return NULL;
}
static Value *scope_lookup(Scope *scope, Str *path, int npath, Arena *a) {
  // single scan: find the scope holding path[0] and return its value (the old
  // code scanned each scope twice: scope_has, then scope_get).
  Scope *s = scope;
  Value *v = NULL;
  while (s) {
    for (int i = s->n - 1; i >= 0; i--) if (str_eq(s->names[i], path[0])) { v = s->vals[i]; break; }
    if (v) break;
    s = s->parent;
  }
  if (!v) return NULL;
  for (int k = 1; k < npath; k++) {
    if (!v) return NULL;
    if (v->kind == V_ARR) {
      // own property of an array: only "length" is a valid path segment
      if (str_eq(path[k], str_from("length"))) { v = v_num(a, (int64_t)v->nitems); continue; }
      return NULL;
    }
    if (v->kind != V_OBJ) return NULL;
    v = v_field(v, path[k]);
    if (!v) return NULL;
  }
  return v;
}
static void text_of_buf(Value *v, Buf *out, Arena *a) {
  if (!v) return;
  switch (v->kind) {
    case V_STR: buf_put_str(out, v->s); break;
    case V_NUM: {
      char tmp[24]; int n = 0;
      int64_t x = v->num;
      if (x == 0) tmp[n++] = '0';
      else {
        int neg = x < 0;
        uint64_t u = neg ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
        char rev[24]; int rn = 0;
        while (u > 0) { rev[rn++] = (char)('0' + u % 10); u /= 10; }
        if (neg) tmp[n++] = '-';
        while (rn > 0) tmp[n++] = rev[--rn];
      }
      buf_put(out, tmp, (size_t)n);
      break;
    }
    case V_BOOL: buf_puts(out, v->boolean ? "true" : "false"); break;
    case V_ARR:
      for (size_t i = 0; i < v->nitems; i++) {
        if (i > 0) buf_puts(out, ", ");
        text_of_buf(v->items[i], out, a);
      }
      break;
    default: break; // undef, null, object -> ""
  }
}
static int truthy_v(Value *v) {
  if (!v) return 0;
  if (v->kind == V_ARR) return v->nitems > 0;
  if (v->kind == V_UNDEF || v->kind == V_NULL) return 0;
  if (v->kind == V_BOOL) return v->boolean;
  if (v->kind == V_STR) return v->s.len > 0;
  if (v->kind == V_NUM) return v->num != 0;
  return 1; // object
}
static void render_range(Op *ops, int start, int end, Scope *scope, TmplCtx *tctx, int depth, Buf *out);

// Try to serve an {% include %} from the render memo. Returns 1 if the
// include was rendered (hit or miss-stored) and appended to out, 0 if the
// caller must render it normally (not cacheable).
static int memo_include(Tmpl *tm, Template *t, Scope *scope, TmplCtx *tctx, int depth, Buf *out) {
  Arena *a = tctx->b->arena;
  // Bound names at this render point: the loop variables on the scope chain.
  // A for-loop frame has exactly 2 names, the second being "loop".
  uint64_t bound_mask = 0;
  for (Scope *s = scope; s; s = s->parent) {
    if (s->n == 2 && str_eq(s->names[1], str_from("loop"))) {
      int id0 = memo_name_id(s->names[0]);
      int id1 = memo_id_of_cstr("loop");
      if (id0 < 0 || id1 < 0) return 0;
      bound_mask |= (1ULL << id0) | (1ULL << id1);
    }
  }
  TmplAnalysis *an = memo_analysis_for(tm, bound_mask, &tctx->b->template_map);
  if (!an || an->bad) return 0;
  int id_site = memo_id_of_cstr("site");
  int id_tags = memo_id_of_cstr("tags");
  int id_recent = memo_id_of_cstr("recent");
  int id_loop = memo_id_of_cstr("loop");
  if (id_site < 0 || id_tags < 0 || id_recent < 0 || id_loop < 0) return 0;
  uint64_t globals = (1ULL << id_site) | (1ULL << id_tags) | (1ULL << id_recent);
  if (an->free_mask & ~globals) return 0; // reads a per-render name (page, ...)
  if (an->outer_mask & (1ULL << id_loop)) return 0; // reads the mutating loop object
  Value *dep[4];
  int ndep = 0;
  for (int id = 0; id < MEMO_MAX_NAMES; id++) {
    if (!(an->outer_mask & (1ULL << id))) continue;
    if (ndep >= 4) return 0;
    Value *v = scope_lookup(scope, &g_memo_names[id], 1, a);
    if (!v) return 0;
    dep[ndep++] = v;
  }
  uint64_t h = (uint64_t)(uintptr_t)t * 1099511628211ULL;
  for (int i = 0; i < ndep; i++) h ^= ((uint64_t)(uintptr_t)dep[i] * 1469598103934665603ULL) + (uint64_t)(i + 1);
  size_t slot = (size_t)(h & (MEMO_SLOTS - 1));
  for (int probe = 0; probe < MEMO_SLOTS; probe++) {
    MemoSlot *m = &g_memo[(slot + probe) & (MEMO_SLOTS - 1)];
    if (m->gen != g_memo_gen) {
      Buf tmp; buf_init(&tmp, a, 1024);
      render_range(tm->ops, 0, tm->nops, scope, tctx, depth + 1, &tmp);
      m->gen = g_memo_gen; m->tm = t; m->ndep = ndep;
      for (int i = 0; i < ndep; i++) m->dep[i] = dep[i];
      m->html.p = tmp.buf; m->html.len = tmp.len;
      g_memo_miss++;
      buf_put_str(out, m->html);
      return 1;
    }
    if (m->tm == t && m->ndep == ndep) {
      int eq = 1;
      for (int i = 0; i < ndep; i++) if (m->dep[i] != dep[i]) { eq = 0; break; }
      if (eq) { g_memo_hits++; buf_put_str(out, m->html); return 1; }
    }
  }
  return 0; // memo table full: render normally
}

static void render_range(Op *ops, int start, int end, Scope *scope, TmplCtx *tctx, int depth, Buf *out) {
  Arena *a = tctx->b->arena;
  for (int i = start; i < end; i++) {
    Op *op = &ops[i];
    switch (op->kind) {
      case OP_TEXT: buf_put_str(out, op->text); break;
      case OP_VAR: {
        Value *v = scope_lookup(scope, op->path, op->npath, a);
        if (v && v->kind == V_STR) esc_html_buf(out, v->s);
        else { Buf tmp; buf_init(&tmp, a, 64); text_of_buf(v, &tmp, a); esc_html_buf(out, (Str){ tmp.buf, tmp.len }); }
        break;
      }
      case OP_RAW: {
        Value *v = scope_lookup(scope, op->path, op->npath, a);
        if (v && v->kind == V_STR) buf_put_str(out, v->s);
        else text_of_buf(v, out, a);
        break;
      }
      case OP_IF: {
        Value *v = scope_lookup(scope, op->path, op->npath, a);
        int tv = truthy_v(v);
        int take_then = (tv != op->not);
        int j = op->jump; // else-op or endif-op index
        if (take_then) {
          render_range(ops, i + 1, j, scope, tctx, depth, out);
        } else if (ops[j].kind == OP_ELSE) {
          render_range(ops, j + 1, ops[j].jump, scope, tctx, depth, out);
          i = ops[j].jump; // endif index; loop i++ moves past
          continue;
        }
        i = (ops[j].kind == OP_ELSE) ? ops[j].jump : j;
        break;
      }
      case OP_FOR: {
        Value *v = scope_lookup(scope, op->path, op->npath, a);
        if (v && v->kind == V_ARR) {
          // mutable loop object, shared across iterations
          Value *loop = v_obj(a, NULL, 0);
          Value *lv_index = v_num(a, 0), *lv_first = v_bool(a, 0), *lv_last = v_bool(a, 0), *lv_length = v_num(a, (int64_t)v->nitems);
          Field *lf = arena_alloc(a, 4 * sizeof(Field));
          lf[0].name = str_from("index"); lf[0].v = lv_index;
          lf[1].name = str_from("first"); lf[1].v = lv_first;
          lf[2].name = str_from("last"); lf[2].v = lv_last;
          lf[3].name = str_from("length"); lf[3].v = lv_length;
          loop->fields = lf; loop->nfields = 4;
          Str names[2]; Value *vals[2];
          names[0] = op->varname; vals[0] = NULL;
          names[1] = str_from("loop"); vals[1] = loop;
          Scope child; child.names = names; child.vals = vals; child.n = 2; child.parent = scope;
          for (size_t k = 0; k < v->nitems; k++) {
            vals[0] = v->items[k];
            lv_index->num = (int64_t)(k + 1);
            lv_first->boolean = (k == 0);
            lv_last->boolean = (k == v->nitems - 1);
            render_range(ops, i + 1, op->jump, &child, tctx, depth, out);
          }
        }
        i = op->jump; // endfor index
        break;
      }
      case OP_INCLUDE: {
        if (depth < 8) {
          Template *t = hmap_get(&tctx->b->template_map, op->incname);
          if (t && !t->parse_err) {
            Tmpl *tm = (Tmpl *)t->ops;
            if (!memo_include(tm, t, scope, tctx, depth, out))
              render_range(tm->ops, 0, tm->nops, scope, tctx, depth + 1, out);
          }
        }
        break;
      }
      default: break; // OP_ELSE/OP_ENDIF/OP_ENDFOR: skipped by jumps
    }
  }
}
// render a template by name with vars; returns 1 + html in out, or 0 if missing.
static int render_named(Build *b, Str name, Str *varnames, Value **varvals, int nvars, Buf *out) {
  Arena *a = b->arena;
  Template *t = hmap_get(&b->template_map, name);
  if (!t || t->parse_err) return 0;
  int total = 3 + nvars;
  Str *names = arena_alloc(a, (size_t)total * sizeof(Str));
  Value **vals = arena_alloc(a, (size_t)total * sizeof(Value *));
  names[0] = str_from("site"); vals[0] = b->site_v;
  names[1] = str_from("tags"); vals[1] = b->tags_v;
  names[2] = str_from("recent"); vals[2] = b->recent_v;
  for (int i = 0; i < nvars; i++) { names[3 + i] = varnames[i]; vals[3 + i] = varvals[i]; }
  Scope scope; scope.names = names; scope.vals = vals; scope.n = total; scope.parent = NULL;
  Tmpl *tm = (Tmpl *)t->ops;
  TmplCtx tctx = { b };
  render_range(tm->ops, 0, tm->nops, &scope, &tctx, 0, out);
  return 1;
}

// ---------------------------------------------------------------- build pipeline
static Str dir_of(Str path) {
  size_t i = path.len;
  while (i > 0 && path.p[i-1] != '/') i--;
  if (i == 0) { Str r = { (const uint8_t *)"", 0 }; return r; }
  Str r = { path.p, i - 1 };
  return r;
}
static int cmp_strp(const void *a, const void *b) {
  // qsort comparator for Str (not used; we sort with insertion sort below)
  (void)a; (void)b;
  return 0;
}
// sort an array of RawFile by path (code-unit order), insertion sort (n small)
// Generic quicksort (Hoare partition, median-of-three pivot, insertion sort
// below 16 elements). The old insertion sorts were O(n^2): ~7ms sorting ~1,070
// file paths and ~5ms sorting ~1,100 report lines per run.
typedef int (*qcmp_fn)(const void *a, const void *b);
static void qswap(uint8_t *a, uint8_t *b, size_t size) {
  uint8_t tmp[64];
  __builtin_memcpy(tmp, a, size);
  __builtin_memcpy(a, b, size);
  __builtin_memcpy(b, tmp, size);
}
static void quicksort_rec(uint8_t *base, size_t n, size_t size, qcmp_fn cmp) {
  while (n > 16) {
    uint8_t *lo = base, *hi = base + (n - 1) * size, *mid = base + (n / 2) * size;
    if (cmp(lo, mid) > 0) qswap(lo, mid, size);
    if (cmp(lo, hi) > 0) qswap(lo, hi, size);
    if (cmp(mid, hi) > 0) qswap(mid, hi, size);
    uint8_t *pivot = mid;
    uint8_t *i = lo, *j = hi;
    for (;;) {
      while (cmp(i, pivot) < 0) i += size;
      while (cmp(j, pivot) > 0) j -= size;
      if (i >= j) break;
      qswap(i, j, size);
      if (i == pivot) pivot = j; else if (j == pivot) pivot = i;
      i += size;
      if (j == base) break;
      j -= size;
    }
    size_t left_n = (size_t)(j - base) / size + 1;
    size_t right_n = n - left_n;
    if (left_n < right_n) { quicksort_rec(base, left_n, size, cmp); base = j + size; n = right_n; }
    else { quicksort_rec(j + size, right_n, size, cmp); n = left_n; }
  }
  for (size_t i = 1; i < n; i++) {
    size_t j = i;
    while (j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0) { qswap(base + (j - 1) * size, base + j * size, size); j--; }
  }
}
static int cmp_file_path(const void *a, const void *b) { return str_cmp(((const RawFile *)a)->path, ((const RawFile *)b)->path); }
static int cmp_tmpl_name(const void *a, const void *b) { return str_cmp(((const RawTemplate *)a)->name, ((const RawTemplate *)b)->name); }
static int cmp_str(const void *a, const void *b) { return str_cmp(*(const Str *)a, *(const Str *)b); }
static void sort_files(RawFile *fs, size_t n) { if (n > 1) quicksort_rec((uint8_t *)fs, n, sizeof(RawFile), cmp_file_path); }
static void sort_templates(RawTemplate *ts, size_t n) { if (n > 1) quicksort_rec((uint8_t *)ts, n, sizeof(RawTemplate), cmp_tmpl_name); }
static void sort_strs(Str *arr, size_t n) { if (n > 1) quicksort_rec((uint8_t *)arr, n, sizeof(Str), cmp_str); }
static int cmp_source_date(const void *pa, const void *pb) {
  Source *a = *(Source **)pa, *b = *(Source **)pb;
  int c = str_cmp(b->fm.date, a->fm.date); // newest first
  if (c != 0) return c;
  return str_cmp(a->slug, b->slug);
}
static int cmp_tag_count(const void *pa, const void *pb) {
  Tag *a = *(Tag **)pa, *b = *(Tag **)pb;
  if (a->nposts != b->nposts) return b->nposts - a->nposts;
  return str_cmp(a->slug, b->slug);
}
static Value *link_of(Arena *a, Source *s) {
  if (!s) return v_null(a);
  Field *f = arena_alloc(a, 4 * sizeof(Field));
  f[0].name = str_from("title"); f[0].v = v_str(a, s->fm.title);
  f[1].name = str_from("url"); f[1].v = v_str(a, s->url);
  f[2].name = str_from("date"); f[2].v = v_str(a, s->fm.has_date ? s->fm.date : str_from(""));
  f[3].name = str_from("date_long"); f[3].v = s->fm.has_date ? v_str(a, long_date(a, s->fm.date)) : v_str(a, str_from(""));
  return v_obj(a, f, 4);
}
static Value *card_of(Build *b, Source *s) {
  if (s->card) return s->card;
  Arena *a = b->arena;
  // tags: [{name, slug, url}]
  Value **tagvals = arena_alloc(a, (s->ntagRefs > 0 ? (size_t)s->ntagRefs : 1) * sizeof(Value *));
  for (int i = 0; i < s->ntagRefs; i++) {
    Tag *t = s->tagRefs[i];
    Field *f = arena_alloc(a, 3 * sizeof(Field));
    f[0].name = str_from("name"); f[0].v = v_str(a, t->name);
    f[1].name = str_from("slug"); f[1].v = v_str(a, t->slug);
    f[2].name = str_from("url"); f[2].v = v_str(a, t->url);
    tagvals[i] = v_obj(a, f, 3);
  }
  Field *f = arena_alloc(a, 8 * sizeof(Field));
  f[0].name = str_from("title"); f[0].v = v_str(a, s->fm.title);
  f[1].name = str_from("url"); f[1].v = v_str(a, s->url);
  f[2].name = str_from("date"); f[2].v = v_str(a, s->fm.has_date ? s->fm.date : str_from(""));
  f[3].name = str_from("date_long"); f[3].v = s->fm.has_date ? v_str(a, long_date(a, s->fm.date)) : v_str(a, str_from(""));
  f[4].name = str_from("summary"); f[4].v = v_str(a, s->fm.has_summary ? s->fm.summary : str_from(""));
  f[5].name = str_from("excerpt"); f[5].v = v_str(a, s->excerpt);
  long rm = (words_of(s->body) + 199) / 200;
  if (rm < 1) rm = 1;
  f[6].name = str_from("reading_minutes"); f[6].v = v_num(a, rm);
  f[7].name = str_from("tags"); f[7].v = v_arr(a, tagvals, (size_t)s->ntagRefs);
  Value *c = v_obj(a, f, 8);
  s->card = c;
  return c;
}
static Value *page_of(Build *b, Source *s) {
  Arena *a = b->arena;
  Value *card = card_of(b, s);
  // toc: [{level, id, html}]
  Value **tocvals = arena_alloc(a, (s->ntoc > 0 ? (size_t)s->ntoc : 1) * sizeof(Value *));
  for (int i = 0; i < s->ntoc; i++) {
    Field *f = arena_alloc(a, 3 * sizeof(Field));
    f[0].name = str_from("level"); f[0].v = v_num(a, s->toc[i].level);
    f[1].name = str_from("id"); f[1].v = v_str(a, s->toc[i].id);
    f[2].name = str_from("html"); f[2].v = v_str(a, s->toc[i].html);
    tocvals[i] = v_obj(a, f, 3);
  }
  Field *f = arena_alloc(a, 14 * sizeof(Field));
  // spread card fields first (8), then slug, content, toc, newer, older, is_post
  for (int i = 0; i < 8; i++) f[i] = card->fields[i];
  f[8].name = str_from("slug"); f[8].v = v_str(a, s->slug);
  f[9].name = str_from("content"); f[9].v = v_str(a, s->html);
  f[10].name = str_from("toc"); f[10].v = v_arr(a, tocvals, (size_t)s->ntoc);
  f[11].name = str_from("newer"); f[11].v = link_of(a, s->newer);
  f[12].name = str_from("older"); f[12].v = link_of(a, s->older);
  f[13].name = str_from("is_post"); f[13].v = v_bool(a, s->post);
  return v_obj(a, f, 14);
}
static Value *tag_of(Build *b, Tag *t) {
  Arena *a = b->arena;
  Value **postvals = arena_alloc(a, (t->nposts > 0 ? (size_t)t->nposts : 1) * sizeof(Value *));
  for (int i = 0; i < t->nposts; i++) postvals[i] = card_of(b, t->posts[i]);
  Field *f = arena_alloc(a, 5 * sizeof(Field));
  f[0].name = str_from("name"); f[0].v = v_str(a, t->name);
  f[1].name = str_from("slug"); f[1].v = v_str(a, t->slug);
  f[2].name = str_from("url"); f[2].v = v_str(a, t->url);
  f[3].name = str_from("count"); f[3].v = v_num(a, t->nposts);
  f[4].name = str_from("posts"); f[4].v = v_arr(a, postvals, (size_t)t->nposts);
  return v_obj(a, f, 5);
}

// ---------------------------------------------------------------- build one case
static void build_case(Input *in, Arena *a, Build *b) {
  memo_tables_clear();
  b->arena = a; b->input = in;
  b->nreport = 0; b->report_cap = 0; b->report = NULL;
  b->nsources = 0; b->sources_cap = 0; b->sources = NULL;
  b->nstatics = 0; b->statics = NULL;
  b->nposts = 0; b->posts = NULL;
  b->ntags = 0; b->tags_cap = 0; b->tags = NULL;
  b->nout = 0; b->out_cap = 0; b->out_keys = NULL; b->out_vals = NULL;
  b->site_v = NULL; b->tags_v = NULL; b->recent_v = NULL;
  // base: base_url with trailing slashes dropped
  Str base = in->site.base_url;
  while (base.len > 0 && base.p[base.len-1] == '/') base.len--;
  b->base = base;
  hmap_init(&b->template_map, a, 64);
  hmap_init(&b->by_source, a, 1024);
  hmap_init(&b->static_paths, a, 64);
  hmap_init(&b->broken, a, 64);
  STEP_END(); STEP_BEGIN(0);
  // 2. templates, in code-unit order of name
  sort_templates(in->templates, in->ntemplates);
  b->templates = arena_alloc(a, (in->ntemplates > 0 ? in->ntemplates : 1) * sizeof(Template));
  b->ntemplates = (int)in->ntemplates;
  for (size_t i = 0; i < in->ntemplates; i++) {
    Template *t = &b->templates[i];
    t->name = in->templates[i].name; t->src = in->templates[i].src;
    Tmpl *tm = arena_alloc(a, sizeof(Tmpl));
    t->ops = tm;
    if (!parse_template(t->name, t->src, a, tm)) {
      t->parse_err = 1; t->err_msg = tm->err_msg;
      build_report(b, tm->err_msg);
    } else t->parse_err = 0;
    hmap_put(&b->template_map, t->name, t);
  }
  STEP_END(); STEP_BEGIN(1);
  // 1. sources, in code-unit order of path
  sort_files(in->files, in->nfiles);
  Source *srcs = arena_alloc(a, (in->nfiles > 0 ? in->nfiles : 1) * sizeof(Source));
  int nsrc = 0;
  RawFile *statics = arena_alloc(a, (in->nfiles > 0 ? in->nfiles : 1) * sizeof(RawFile));
  int nstat = 0;
  for (size_t i = 0; i < in->nfiles; i++) {
    RawFile *f = &in->files[i];
    // ends with .md?
    if (f->path.len >= 3 && f->path.p[f->path.len-3] == '.' && f->path.p[f->path.len-2] == 'm' && f->path.p[f->path.len-1] == 'd') {
      Source *s = &srcs[nsrc++];
      s->path = f->path;
      // \r\n? -> \n (only if a \r is present)
      int has_cr = 0;
      for (size_t k = 0; k < f->content.len; k++) if (f->content.p[k] == '\r') { has_cr = 1; break; }
      if (has_cr) {
        uint8_t *o = arena_alloc(a, f->content.len + 1);
        size_t n = 0;
        for (size_t k = 0; k < f->content.len; k++) {
          uint8_t c = f->content.p[k];
          if (c == '\r') { if (k + 1 < f->content.len && f->content.p[k+1] == '\n') continue; o[n++] = '\n'; }
          else o[n++] = c;
        }
        s->text.p = o; s->text.len = n;
      } else s->text = f->content;
      s->newer = NULL; s->older = NULL; s->card = NULL; s->tagRefs = NULL; s->ntagRefs = 0;
      s->toc = NULL; s->ntoc = 0;
    } else {
      statics[nstat++] = *f;
      strset_add(&b->static_paths, f->path);
    }
  }
  b->statics = statics; b->nstatics = nstat;
  STEP_END(); STEP_BEGIN(2);
  // 2. front matter
  Source *kept = arena_alloc(a, (nsrc > 0 ? nsrc : 1) * sizeof(Source));
  int nkept = 0;
  for (int i = 0; i < nsrc; i++) {
    Source *s = &srcs[i];
    Str err;
    if (!parse_front_matter(s->text, a, &s->fm, &s->body, &err)) {
      Buf rb; buf_init(&rb, a, 128);
      buf_puts(&rb, "refused ");
      buf_put_str(&rb, s->path);
      buf_puts(&rb, ": ");
      buf_put_str(&rb, err);
      build_report(b, (Str){ rb.buf, rb.len });
      continue;
    }
    s->post = (s->path.len >= 6 && __builtin_memcmp(s->path.p, "posts/", 6) == 0);
    // reserved dirs tags/ and page/
    const char *own = NULL;
    if (s->path.len >= 5 && __builtin_memcmp(s->path.p, "tags/", 5) == 0) own = "tags";
    else if (s->path.len >= 5 && __builtin_memcmp(s->path.p, "page/", 5) == 0) own = "page";
    if (own) {
      Buf rb; buf_init(&rb, a, 128);
      buf_puts(&rb, "refused ");
      buf_put_str(&rb, s->path);
      buf_puts(&rb, ": ");
      buf_puts(&rb, own);
      buf_puts(&rb, "/ is the generator's own");
      build_report(b, (Str){ rb.buf, rb.len });
      continue;
    }
    if (s->post && !s->fm.has_date) {
      Buf rb; buf_init(&rb, a, 128);
      buf_puts(&rb, "refused ");
      buf_put_str(&rb, s->path);
      buf_puts(&rb, ": a post needs a date");
      build_report(b, (Str){ rb.buf, rb.len });
      continue;
    }
    Str layout = s->fm.has_layout ? s->fm.layout : (s->post ? str_from("post") : str_from("page"));
    if (!hmap_get(&b->template_map, layout) || ((Template *)hmap_get(&b->template_map, layout))->parse_err) {
      Buf rb; buf_init(&rb, a, 128);
      buf_puts(&rb, "refused ");
      buf_put_str(&rb, s->path);
      buf_puts(&rb, ": layout \"");
      buf_put_str(&rb, layout);
      buf_puts(&rb, "\" is no template that parses");
      build_report(b, (Str){ rb.buf, rb.len });
      continue;
    }
    s->layout = layout;
    if (s->fm.has_draft && s->fm.draft) {
      Buf rb; buf_init(&rb, a, 128);
      buf_puts(&rb, "draft ");
      buf_put_str(&rb, s->path);
      build_report(b, (Str){ rb.buf, rb.len });
      continue;
    }
    kept[nkept++] = *s;
  }
  b->sources = kept; b->nsources = nkept;
  STEP_END(); STEP_BEGIN(3);
  // 3. permalinks
  HMap taken_map; hmap_init(&taken_map, a, 64);
  for (int i = 0; i < nkept; i++) {
    Source *s = &b->sources[i];
    Str dir = s->post ? str_from("posts") : dir_of(s->path);
    StrSet *taken = hmap_get(&taken_map, dir);
    if (!taken) {
      taken = arena_alloc(a, sizeof(StrSet));
      hmap_init(taken, a, 64);
      if (dir.len == 0) {
        strset_add(taken, str_from("posts"));
        strset_add(taken, str_from("tags"));
        strset_add(taken, str_from("page"));
      }
      hmap_put(&taken_map, dir, taken);
    }
    Str slug = s->fm.has_slug ? s->fm.slug : slugify(a, s->fm.title);
    slug = unique_slug(a, slug, taken);
    s->slug = slug;
    if (dir.len == 0) s->outDir = slug;
    else {
      uint8_t *o = arena_alloc(a, dir.len + 1 + slug.len);
      size_t n = 0;
      for (size_t k = 0; k < dir.len; k++) o[n++] = dir.p[k];
      o[n++] = '/';
      for (size_t k = 0; k < slug.len; k++) o[n++] = slug.p[k];
      s->outDir.p = o; s->outDir.len = n;
    }
    {
      uint8_t *o = arena_alloc(a, b->base.len + 1 + s->outDir.len + 1);
      size_t n = 0;
      for (size_t k = 0; k < b->base.len; k++) o[n++] = b->base.p[k];
      o[n++] = '/';
      for (size_t k = 0; k < s->outDir.len; k++) o[n++] = s->outDir.p[k];
      o[n++] = '/';
      s->url.p = o; s->url.len = n;
    }
    hmap_put(&b->by_source, s->path, s);
  }
  STEP_END(); STEP_BEGIN(4);
  // 4. markdown
  link_memo_clear();
  for (int i = 0; i < nkept; i++) {
    Source *s = &b->sources[i];
    LinkCtx lctx; lctx.b = b; lctx.dir = dir_of(s->path); lctx.src_path = s->path;
    Buf hb; buf_init(&hb, a, 4096);
    MdOut mdout; mdout.items = NULL; mdout.n = 0; mdout.cap = 0; mdout.arena = a;
    mdout.excerpt.p = NULL; mdout.excerpt.len = 0; mdout.has_excerpt = 0;
    render_markdown(s->body, &lctx, &hb, &mdout);
    s->html.p = hb.buf; s->html.len = hb.len;
    s->toc = mdout.items; s->ntoc = mdout.n;
    s->excerpt = mdout.has_excerpt ? mdout.excerpt : str_from("");
  }
  STEP_END(); STEP_BEGIN(5);
  // 5. collections
  Source **posts = arena_alloc(a, (nkept > 0 ? nkept : 1) * sizeof(Source *));
  int nposts = 0;
  for (int i = 0; i < nkept; i++) if (b->sources[i].post) posts[nposts++] = &b->sources[i];
  // sort: date desc, slug asc — use qsort via a wrapper (glibc qsort is libc;
  // use insertion sort: nposts is a few hundred)
  for (int i = 1; i < nposts; i++) {
    Source *t = posts[i];
    int j = i;
    while (j > 0) {
      // posts[j-1] > t (date desc, then slug asc) means shift
      int gt = 0;
      int cd = str_cmp(posts[j-1]->fm.date, t->fm.date);
      if (cd < 0) gt = 1; // posts[j-1] date older -> t (newer) goes first -> shift
      else if (cd == 0) { int cs = str_cmp(posts[j-1]->slug, t->slug); if (cs > 0) gt = 1; }
      if (!gt) break;
      posts[j] = posts[j-1]; j--;
    }
    posts[j] = t;
  }
  b->posts = posts; b->nposts = nposts;
  for (int k = 0; k < nposts; k++) {
    posts[k]->newer = k == 0 ? NULL : posts[k-1];
    posts[k]->older = k == nposts - 1 ? NULL : posts[k+1];
  }
  STEP_END(); STEP_BEGIN(6);
  // 6. tags
  HMap by_slug; hmap_init(&by_slug, a, 64);
  // Pre-size: distinct slugs <= total tag occurrences across posts, so the
  // array NEVER grows. (It used to grow while by_slug held Tag* pointers
  // into it — the grow abandoned the old block and all counts/posts written
  // through by_slug landed on stale copies: wrong tag counts and tag lists.)
  int tags_cap = 16;
  for (int k = 0; k < nposts; k++) tags_cap += (int)posts[k]->fm.ntags;
  Tag *tags = arena_alloc(a, (size_t)tags_cap * sizeof(Tag));
  int ntags = 0;
  for (int k = 0; k < nposts; k++) {
    Source *p = posts[k];
    // ownTags: each slug once, first spelling kept, in its own order
    StrSet seen; hmap_init(&seen, a, 16);
    Tag **refs = arena_alloc(a, (p->fm.ntags > 0 ? p->fm.ntags : 1) * sizeof(Tag *));
    int nrefs = 0;
    for (size_t ti = 0; ti < p->fm.ntags; ti++) {
      Str name = p->fm.tags[ti];
      Str slug = slugify(a, name);
      if (strset_has(&seen, slug)) continue;
      strset_add(&seen, slug);
      Tag *tg = hmap_get(&by_slug, slug);
      if (!tg) {
        if (ntags >= tags_cap) __builtin_trap(); // pre-sized above; cannot happen
        tg = &tags[ntags++];
        tg->name = name; tg->slug = slug;
        {
          uint8_t *o = arena_alloc(a, b->base.len + 6 + slug.len + 1);
          size_t n = 0;
          for (size_t q = 0; q < b->base.len; q++) o[n++] = b->base.p[q];
          const char *mid = "/tags/";
          for (int q = 0; mid[q]; q++) o[n++] = (uint8_t)mid[q];
          for (size_t q = 0; q < slug.len; q++) o[n++] = slug.p[q];
          o[n++] = '/';
          tg->url.p = o; tg->url.len = n;
        }
        tg->posts = NULL; tg->nposts = 0; tg->posts_cap = 0;
        hmap_put(&by_slug, slug, tg);
      }
      if (tg->nposts == tg->posts_cap) {
        tg->posts_cap = tg->posts_cap ? tg->posts_cap * 2 : 8;
        Source **np = arena_alloc(a, (size_t)tg->posts_cap * sizeof(Source *));
        for (int q = 0; q < tg->nposts; q++) np[q] = tg->posts[q];
        tg->posts = np;
      }
      tg->posts[tg->nposts++] = p;
      refs[nrefs++] = tg;
    }
    p->tagRefs = refs; p->ntagRefs = nrefs;
  }
  for (int i = 0; i < nkept; i++) {
    Source *s = &b->sources[i];
    if (s->post) continue;
    StrSet seen; hmap_init(&seen, a, 16);
    Tag **refs = arena_alloc(a, (s->fm.ntags > 0 ? s->fm.ntags : 1) * sizeof(Tag *));
    int nrefs = 0;
    for (size_t ti = 0; ti < s->fm.ntags; ti++) {
      Str name = s->fm.tags[ti];
      Str slug = slugify(a, name);
      if (strset_has(&seen, slug)) continue;
      strset_add(&seen, slug);
      Tag *tg = hmap_get(&by_slug, slug);
      if (!tg) {
        // synthetic tag: {name, slug, url: '', posts: []}
        Tag *syn = arena_alloc(a, sizeof(Tag));
        syn->name = name; syn->slug = slug; syn->url = str_from("");
        syn->posts = NULL; syn->nposts = 0; syn->posts_cap = 0;
        refs[nrefs++] = syn;
      } else refs[nrefs++] = tg;
    }
    s->tagRefs = refs; s->ntagRefs = nrefs;
  }
  // Sorted view (count desc, slug asc) for the `tags` template value. The
  // structs themselves are NEVER moved: by_slug and every post's tagRefs
  // hold Tag* pointers into this array, and sorting the structs in place
  // would scramble them all (that was a real bug: per-page tag lists showed
  // other posts' tags after the sort).
  Tag **torder = arena_alloc(a, (ntags > 0 ? (size_t)ntags : 1) * sizeof(Tag *));
  for (int i = 0; i < ntags; i++) torder[i] = &tags[i];
  for (int i = 1; i < ntags; i++) {
    Tag *t = torder[i];
    int j = i;
    while (j > 0) {
      int gt = 0;
      if (torder[j-1]->nposts < t->nposts) gt = 1;
      else if (torder[j-1]->nposts == t->nposts && str_cmp(torder[j-1]->slug, t->slug) > 0) gt = 1;
      if (!gt) break;
      torder[j] = torder[j-1]; j--;
    }
    torder[j] = t;
  }
  b->tags = tags; b->ntags = ntags;
  STEP_END(); STEP_BEGIN(7);
  // case-constant template values
  {
    Field *f = arena_alloc(a, 2 * sizeof(Field));
    f[0].name = str_from("title"); f[0].v = v_str(a, in->site.title);
    f[1].name = str_from("base_url"); f[1].v = v_str(a, b->base);
    b->site_v = v_obj(a, f, 2);
    Value **allTagVals = arena_alloc(a, (ntags > 0 ? (size_t)ntags : 1) * sizeof(Value *));
    for (int i = 0; i < ntags; i++) {
      Field *tf = arena_alloc(a, 4 * sizeof(Field));
      tf[0].name = str_from("name"); tf[0].v = v_str(a, torder[i]->name);
      tf[1].name = str_from("slug"); tf[1].v = v_str(a, torder[i]->slug);
      tf[2].name = str_from("url"); tf[2].v = v_str(a, torder[i]->url);
      tf[3].name = str_from("count"); tf[3].v = v_num(a, torder[i]->nposts);
      allTagVals[i] = v_obj(a, tf, 4);
    }
    b->tags_v = v_arr(a, allTagVals, (size_t)ntags);
    int rn = nposts < (int)in->site.recent ? nposts : (int)in->site.recent;
    Value **recentVals = arena_alloc(a, (rn > 0 ? (size_t)rn : 1) * sizeof(Value *));
    for (int i = 0; i < rn; i++) recentVals[i] = card_of(b, posts[i]);
    b->recent_v = v_arr(a, recentVals, (size_t)rn);
  }
  STEP_END(); STEP_BEGIN(8);
  // 7. layouts
  for (int i = 0; i < nkept; i++) {
    Source *s = &b->sources[i];
    Buf ob; buf_init(&ob, a, 8192);
    Str vn[1]; Value *vv[1];
    vn[0] = str_from("page"); vv[0] = page_of(b, s);
    if (render_named(b, s->layout, vn, vv, 1, &ob)) {
      uint8_t *kp = arena_alloc(a, s->outDir.len + 12);
      size_t kn = 0;
      for (size_t q = 0; q < s->outDir.len; q++) kp[kn++] = s->outDir.p[q];
      const char *suf = "/index.html";
      for (int q = 0; suf[q]; q++) kp[kn++] = (uint8_t)suf[q];
      build_out(b, (Str){ kp, kn }, (Str){ ob.buf, ob.len });
    }
  }
  {
    int64_t per = in->site.per_page;
    int pages = 1;
    if (nposts > 0) pages = (int)((nposts + per - 1) / per);
    if (pages < 1) pages = 1;
    for (int k = 1; k <= pages; k++) {
      Buf ob; buf_init(&ob, a, 16384);
      int start = (k - 1) * (int)per;
      int cnt = nposts - start;
      if (cnt > per) cnt = (int)per;
      if (cnt < 0) cnt = 0;
      Value **pv = arena_alloc(a, (cnt > 0 ? (size_t)cnt : 1) * sizeof(Value *));
      for (int q = 0; q < cnt; q++) pv[q] = card_of(b, posts[start + q]);
      Value *posts_v = v_arr(a, pv, (size_t)cnt);
      // pager
      Str idx_url;
      {
        uint8_t *o = arena_alloc(a, b->base.len + 32);
        size_t n = 0;
        for (size_t q = 0; q < b->base.len; q++) o[n++] = b->base.p[q];
        o[n++] = '/';
        if (k != 1) {
          const char *mid = "page/";
          for (int q = 0; mid[q]; q++) o[n++] = (uint8_t)mid[q];
          char num[16]; int nn = 0; int v = k; char tmp[16]; int tn = 0;
          if (v == 0) tmp[tn++] = '0';
          while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
          while (tn > 0) num[nn++] = tmp[--tn];
          for (int q = 0; q < nn; q++) o[n++] = (uint8_t)num[q];
          o[n++] = '/';
        }
        idx_url.p = o; idx_url.len = n;
      }
      Str newer_url = str_from("");
      if (k != 1) {
        uint8_t *o = arena_alloc(a, b->base.len + 32);
        size_t n = 0;
        for (size_t q = 0; q < b->base.len; q++) o[n++] = b->base.p[q];
        o[n++] = '/';
        if (k - 1 != 1) {
          const char *mid = "page/";
          for (int q = 0; mid[q]; q++) o[n++] = (uint8_t)mid[q];
          char num[16]; int nn = 0; int v = k - 1; char tmp[16]; int tn = 0;
          if (v == 0) tmp[tn++] = '0';
          while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
          while (tn > 0) num[nn++] = tmp[--tn];
          for (int q = 0; q < nn; q++) o[n++] = (uint8_t)num[q];
          o[n++] = '/';
        }
        newer_url.p = o; newer_url.len = n;
      }
      Str older_url = str_from("");
      if (k != pages) {
        uint8_t *o = arena_alloc(a, b->base.len + 32);
        size_t n = 0;
        for (size_t q = 0; q < b->base.len; q++) o[n++] = b->base.p[q];
        o[n++] = '/';
        const char *mid = "page/";
        for (int q = 0; mid[q]; q++) o[n++] = (uint8_t)mid[q];
        char num[16]; int nn = 0; int v = k + 1; char tmp[16]; int tn = 0;
        if (v == 0) tmp[tn++] = '0';
        while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
        while (tn > 0) num[nn++] = tmp[--tn];
        for (int q = 0; q < nn; q++) o[n++] = (uint8_t)num[q];
        o[n++] = '/';
        older_url.p = o; older_url.len = n;
      }
      Field *pf = arena_alloc(a, 5 * sizeof(Field));
      pf[0].name = str_from("number"); pf[0].v = v_num(a, k);
      pf[1].name = str_from("total"); pf[1].v = v_num(a, pages);
      pf[2].name = str_from("url"); pf[2].v = v_str(a, idx_url);
      pf[3].name = str_from("newer_url"); pf[3].v = v_str(a, newer_url);
      pf[4].name = str_from("older_url"); pf[4].v = v_str(a, older_url);
      Value *pager = v_obj(a, pf, 5);
      Str vn[2]; Value *vv[2];
      vn[0] = str_from("posts"); vv[0] = posts_v;
      vn[1] = str_from("pager"); vv[1] = pager;
      if (render_named(b, str_from("index"), vn, vv, 2, &ob)) {
        if (k == 1) build_out(b, str_from("index.html"), (Str){ ob.buf, ob.len });
        else {
          uint8_t *kp = arena_alloc(a, 32);
          size_t kn = 0;
          const char *pre = "page/";
          for (int q = 0; pre[q]; q++) kp[kn++] = (uint8_t)pre[q];
          char num[16]; int nn = 0; int v = k; char tmp[16]; int tn = 0;
          if (v == 0) tmp[tn++] = '0';
          while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
          while (tn > 0) num[nn++] = tmp[--tn];
          for (int q = 0; q < nn; q++) kp[kn++] = (uint8_t)num[q];
          const char *suf = "/index.html";
          for (int q = 0; suf[q]; q++) kp[kn++] = (uint8_t)suf[q];
          build_out(b, (Str){ kp, kn }, (Str){ ob.buf, ob.len });
        }
      }
    }
  }
  for (int ti = 0; ti < ntags; ti++) {
    Tag *t = &tags[ti];
    Buf ob; buf_init(&ob, a, 8192);
    Str vn[1]; Value *vv[1];
    vn[0] = str_from("tag"); vv[0] = tag_of(b, t);
    if (render_named(b, str_from("tag"), vn, vv, 1, &ob)) {
      uint8_t *kp = arena_alloc(a, 6 + t->slug.len + 12);
      size_t kn = 0;
      const char *pre = "tags/";
      for (int q = 0; pre[q]; q++) kp[kn++] = (uint8_t)pre[q];
      for (size_t q = 0; q < t->slug.len; q++) kp[kn++] = t->slug.p[q];
      const char *suf = "/index.html";
      for (int q = 0; suf[q]; q++) kp[kn++] = (uint8_t)suf[q];
      build_out(b, (Str){ kp, kn }, (Str){ ob.buf, ob.len });
    }
  }
  {
    Buf ob; buf_init(&ob, a, 4096);
    if (render_named(b, str_from("tags"), NULL, NULL, 0, &ob)) {
      build_out(b, str_from("tags/index.html"), (Str){ ob.buf, ob.len });
    }
  }
  for (int i = 0; i < nstat; i++) {
    // shadowed if a built page has this path: check output keys
    int shadowed = 0;
    for (int q = 0; q < b->nout; q++) if (str_eq(b->out_keys[q], statics[i].path)) { shadowed = 1; break; }
    if (shadowed) {
      Buf rb; buf_init(&rb, a, 128);
      buf_puts(&rb, "shadowed ");
      buf_put_str(&rb, statics[i].path);
      buf_puts(&rb, ": a built page has its path");
      build_report(b, (Str){ rb.buf, rb.len });
    } else build_out(b, statics[i].path, statics[i].content);
  }
  STEP_END(); STEP_BEGIN(9);
  // 8. report
  {
    // collect broken messages
    int nb = 0;
    for (size_t i = 0; i < b->broken.cap; i++) if (b->broken.ents[i].used) nb++;
    Str *all = arena_alloc(a, (size_t)(b->nreport + nb + 1) * sizeof(Str));
    int n = 0;
    for (int i = 0; i < b->nreport; i++) all[n++] = b->report[i];
    for (size_t i = 0; i < b->broken.cap; i++) if (b->broken.ents[i].used) all[n++] = b->broken.ents[i].key;
    sort_strs(all, (size_t)n);
    Buf rb; buf_init(&rb, a, 4096);
    for (int i = 0; i < n; i++) { buf_put_str(&rb, all[i]); buf_putc(&rb, '\n'); }
    int pages = 0;
    for (int i = 0; i < nkept; i++) if (!b->sources[i].post) pages++;
    char counts[128];
    int cn = 0;
    const char *pre = "built ";
    while (*pre) counts[cn++] = *pre++;
    // pages
    {
      char tmp[16]; int tn = 0; int v = pages;
      if (v == 0) tmp[tn++] = '0';
      while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
      while (tn > 0) counts[cn++] = tmp[--tn];
    }
    const char *m1 = " pages, ";
    while (*m1) counts[cn++] = *m1++;
    {
      char tmp[16]; int tn = 0; int v = nposts;
      if (v == 0) tmp[tn++] = '0';
      while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
      while (tn > 0) counts[cn++] = tmp[--tn];
    }
    const char *m2 = " posts, ";
    while (*m2) counts[cn++] = *m2++;
    {
      char tmp[16]; int tn = 0; int v = ntags;
      if (v == 0) tmp[tn++] = '0';
      while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
      while (tn > 0) counts[cn++] = tmp[--tn];
    }
    const char *m3 = " tags and ";
    while (*m3) counts[cn++] = *m3++;
    {
      char tmp[16]; int tn = 0; int v = nstat;
      if (v == 0) tmp[tn++] = '0';
      while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
      while (tn > 0) counts[cn++] = tmp[--tn];
    }
    const char *m4 = " static files";
    while (*m4) counts[cn++] = *m4++;
    buf_put(&rb, counts, (size_t)cn);
    buf_putc(&rb, '\n');
    build_out(b, str_from("build-report.txt"), (Str){ rb.buf, rb.len });
  }
  STEP_END();
}

// emit one case's answer object as JSON text.
static void emit_case_json(Buf *out, Build *b) {
  buf_putc(out, '{');
  for (int i = 0; i < b->nout; i++) {
    if (i) buf_putc(out, ',');
    json_emit_str(out, b->out_keys[i]);
    buf_putc(out, ':');
    json_emit_str(out, b->out_vals[i]);
  }
  buf_putc(out, '}');
}
// Run all cases from a cases.json buffer; emit answers.json bytes into out.
// The arena is NOT reset between cases: parsed inputs and the output buffer
// all live in it. Size it generously (256MB virtual; only touched pages cost).
static int run_cases(const uint8_t *data, size_t len, Arena *arena, Buf *out) {
  Input *inputs = NULL;
  int n = parse_cases(data, len, arena, &inputs);
  if (n < 0) return -1;
  buf_putc(out, '[');
  for (int i = 0; i < n; i++) {
    if (i) buf_putc(out, ',');
    Build b;
    build_case(&inputs[i], arena, &b);
    emit_case_json(out, &b);
  }
  buf_putc(out, ']');
  return n;
}



// ---- public shims for the N-API layer (engine.c internals are static) ----
Arena *engine_alloc_arena(size_t bytes) {
  Arena *a = (Arena *)__builtin_malloc(sizeof(Arena));
  if (!a) return NULL;
  arena_init(a, bytes);
  return a;
}
void engine_free_arena(Arena *a) {
  if (!a) return;
  arena_free(a);
  __builtin_free(a);
}
uint8_t *engine_arena_alloc(Arena *a, size_t n) { return (uint8_t *)arena_alloc(a, n); }
Buf *engine_new_buf(Arena *a, size_t cap) {
  Buf *b = (Buf *)arena_alloc(a, sizeof(Buf));
  if (!b) return NULL;
  buf_init(b, a, cap);
  return b;
}
uint8_t *engine_buf_data(Buf *b) { return b->buf; }
size_t engine_buf_len(Buf *b) { return b->len; }

// ---- public entry points for the N-API layer (and test main) ----
// run the full pipeline on cases.json bytes in memory; answers JSON text in out.
// returns number of cases, or -1 on parse failure.
int engine_run(const uint8_t *data, size_t len, Buf *out, Arena *arena) {
  Input *inputs = NULL;
  int n = parse_cases(data, len, arena, &inputs);
  if (n < 0) return -1;
  buf_putc(out, '[');
  for (int i = 0; i < n; i++) {
    if (i) buf_putc(out, ',');
    Build b;
    build_case(&inputs[i], arena, &b);
    emit_case_json(out, &b);
  }
  buf_putc(out, ']');
  return n;
}
// run the full pipeline on a cases.json FILE; write answers.json bytes to out_path.
// answers JSON text also in out. returns number of cases, or -1 on failure.
int engine_run_file(const char *in_path, const char *out_path, Buf *out, Arena *arena) {
  size_t len; uint8_t *data = map_file(in_path, &len);
  if (!data) return -1;
  int n = engine_run(data, len, out, arena);
  if (n < 0) return -1;
  long fd = sys3(2, (long)out_path, 0101 | 01000 | 01, 0644); // O_WRONLY|O_CREAT|O_TRUNC
  if (fd < 0) return -1;
  if (write_all(fd, out->buf, out->len) < 0) { sys1(3, fd); return -1; }
  sys1(3, fd);
  return n;
}

#ifdef ENGINE_TEST_MAIN
// ---- standalone CLI (test builds only; not in the .node) ----
//   engine_test md <bodyfile> <envfile>   -> {"html","excerpt","toc"} JSON on stdout
//     envfile lines: "base <url>" / "dir [<dir>]" / "src <path>" /
//                    "S <path>\t<url>" (by_source) / "T <path>" (static)
//   engine_test build <cases.json>        -> answers.json bytes on stdout
//   engine_test bench <cases.json> <iters> -> per-iter us + checksum on stdout
#define SYS_EXIT 60
#define SYS_CLOCK_GETTIME 228
static int64_t tm_now_us(void) { return (int64_t)t_now_us(); }
static void tm_die(const char *m) { write_all(2, (const uint8_t *)m, __builtin_strlen(m)); sys1(SYS_EXIT, 1); }
static void tm_put_int(Buf *b, long v) {
  char tmp[32]; int n = 0;
  if (v == 0) { buf_putc(b, '0'); return; }
  if (v < 0) { buf_putc(b, '-'); v = -v; }
  while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
  while (n > 0) buf_putc(b, (uint8_t)tmp[--n]);
}
static void tm_put_u64(Buf *b, uint64_t v) {
  char tmp[32]; int n = 0;
  if (v == 0) { buf_putc(b, '0'); return; }
  while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
  while (n > 0) buf_putc(b, (uint8_t)tmp[--n]);
}
static int tm_starts(const uint8_t *p, size_t len, const char *pre) {
  size_t n = __builtin_strlen(pre);
  if (len < n) return 0;
  return __builtin_memcmp(p, pre, n) == 0;
}
static long tm_atoi(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } return v; }

static int cmd_md(int argc, char **argv) {
  if (argc < 4) tm_die("usage: engine_test md <bodyfile> <envfile>\n");
  size_t blen; uint8_t *body = map_file(argv[2], &blen);
  if (!body) tm_die("md: cannot map body\n");
  size_t elen; uint8_t *env = map_file(argv[3], &elen);
  if (!env) tm_die("md: cannot map env\n");
  Arena arena; arena_init(&arena, 64 << 20);
  Str base = { 0, 0 }, dir = { 0, 0 }, src_path = { 0, 0 };
  int ns = 0, nt = 0;
  // pass 1: count S/T lines; pass 2: fill
  for (int pass = 0; pass < 2; pass++) {
    size_t p = 0;
    int si = 0, ti = 0;
    Str *spaths = NULL, *surls = NULL, *tpaths = NULL;
    if (pass == 1) {
      spaths = arena_alloc(&arena, (size_t)(ns > 0 ? ns : 1) * sizeof(Str));
      surls = arena_alloc(&arena, (size_t)(ns > 0 ? ns : 1) * sizeof(Str));
      tpaths = arena_alloc(&arena, (size_t)(nt > 0 ? nt : 1) * sizeof(Str));
    }
    while (p < elen) {
      size_t e = p;
      while (e < elen && env[e] != '\n') e++;
      size_t ll = e - p;
      if (ll > 0 && env[p + ll - 1] == '\r') ll--;
      const uint8_t *l = env + p;
      if (tm_starts(l, ll, "base ")) { base.p = l + 5; base.len = ll - 5; }
      else if (ll == 3 && tm_starts(l, ll, "dir")) { dir.p = (const uint8_t *)""; dir.len = 0; }
      else if (tm_starts(l, ll, "dir ")) { dir.p = l + 4; dir.len = ll - 4; }
      else if (tm_starts(l, ll, "src ")) { src_path.p = l + 4; src_path.len = ll - 4; }
      else if (tm_starts(l, ll, "S ")) {
        if (pass == 0) ns++;
        else {
          size_t t = 2;
          while (t < ll && l[t] != '\t') t++;
          spaths[si].p = l + 2; spaths[si].len = t - 2;
          surls[si].p = l + t + 1; surls[si].len = ll - (t + 1);
          si++;
        }
      } else if (tm_starts(l, ll, "T ")) {
        if (pass == 0) nt++;
        else { tpaths[ti].p = l + 2; tpaths[ti].len = ll - 2; ti++; }
      }
      p = e + 1;
    }
    if (pass == 1) {
      Build b; __builtin_memset(&b, 0, sizeof b);
      b.arena = &arena; b.base = base;
      hmap_init(&b.by_source, &arena, 1024);
      hmap_init(&b.static_paths, &arena, 1024);
      hmap_init(&b.broken, &arena, 64);
      for (int i = 0; i < ns; i++) {
        Source *s = arena_alloc(&arena, sizeof(Source));
        __builtin_memset(s, 0, sizeof *s);
        s->path = spaths[i]; s->url = surls[i];
        hmap_put(&b.by_source, spaths[i], s);
      }
      for (int i = 0; i < nt; i++) strset_add(&b.static_paths, tpaths[i]);
      LinkCtx lctx; lctx.b = &b; lctx.dir = dir; lctx.src_path = src_path;
      Buf hb; buf_init(&hb, &arena, 4096);
      MdOut mdout; mdout.items = NULL; mdout.n = 0; mdout.cap = 0; mdout.arena = &arena;
      mdout.excerpt.p = NULL; mdout.excerpt.len = 0; mdout.has_excerpt = 0;
      render_markdown((Str){ body, blen }, &lctx, &hb, &mdout);
      Buf ob; buf_init(&ob, &arena, hb.len + 1024);
      buf_puts(&ob, "{\"html\":");
      json_emit_str(&ob, (Str){ hb.buf, hb.len });
      buf_puts(&ob, ",\"excerpt\":");
      json_emit_str(&ob, mdout.has_excerpt ? mdout.excerpt : str_from(""));
      buf_puts(&ob, ",\"toc\":[");
      for (int i = 0; i < mdout.n; i++) {
        if (i) buf_putc(&ob, ',');
        buf_puts(&ob, "{\"level\":");
        tm_put_int(&ob, mdout.items[i].level);
        buf_puts(&ob, ",\"id\":");
        json_emit_str(&ob, mdout.items[i].id);
        buf_puts(&ob, ",\"html\":");
        json_emit_str(&ob, mdout.items[i].html);
        buf_putc(&ob, '}');
      }
      buf_puts(&ob, "]}");
      write_all(1, ob.buf, ob.len);
      return 0;
    }
  }
  return 0;
}

static int cmd_build(int argc, char **argv) {
  if (argc < 3) tm_die("usage: engine_test build <cases.json>\n");
  size_t len; uint8_t *data = map_file(argv[2], &len);
  if (!data) tm_die("build: cannot map cases.json\n");
  Arena arena; arena_init(&arena, 256 << 20);
  Buf out; buf_init(&out, &arena, 1 << 20);
  int n = run_cases(data, len, &arena, &out);
  if (n < 0) tm_die("build: parse failed\n");
  write_all(1, out.buf, out.len);
  return 0;
}

static int cmd_bench(int argc, char **argv) {
  if (argc < 4) tm_die("usage: engine_test bench <cases.json> <iters>\n");
  size_t len; uint8_t *data = map_file(argv[2], &len);
  if (!data) tm_die("bench: cannot map cases.json\n");
  long iters = tm_atoi(argv[3]);
  if (iters < 1) iters = 1;
  Arena oarena; arena_init(&oarena, 1 << 20);
  Buf ob; buf_init(&ob, &oarena, 4096);
  int64_t total = 0;
  for (long it = 0; it < iters; it++) {
    Arena arena; arena_init(&arena, 256 << 20);
    Buf out; buf_init(&out, &arena, 1 << 20);
    int64_t t0 = tm_now_us();
    int n = run_cases(data, len, &arena, &out);
    int64_t t1 = tm_now_us();
    if (n < 0) tm_die("bench: parse failed\n");
    uint64_t sum = 1469598103934665603ULL;
    for (size_t i = 0; i < out.len; i++) { sum ^= out.buf[i]; sum *= 1099511628211ULL; }
    int64_t us = t1 - t0;
    total += us;
    buf_puts(&ob, "iter ");
    tm_put_int(&ob, it);
    buf_puts(&ob, ": ");
    tm_put_u64(&ob, (uint64_t)us);
    buf_puts(&ob, " us cases=");
    tm_put_int(&ob, n);
    buf_puts(&ob, " bytes=");
    tm_put_u64(&ob, (uint64_t)out.len);
    buf_puts(&ob, " sum=");
    tm_put_u64(&ob, sum);
    buf_putc(&ob, '\n');
    arena_free(&arena);
  }
  buf_puts(&ob, "total_us=");
  tm_put_u64(&ob, (uint64_t)total);
  buf_puts(&ob, " avg_us=");
  tm_put_u64(&ob, (uint64_t)(total / iters));
  buf_putc(&ob, '\n');
  write_all(1, ob.buf, ob.len);
  return 0;
}


static int cmd_dump(int argc, char **argv) {
  if (argc < 3) tm_die("usage: engine_test dump <cases.json>\n");
  size_t len; uint8_t *data = map_file(argv[2], &len);
  if (!data) tm_die("dump: cannot map cases.json\n");
  Arena arena; arena_init(&arena, 256 << 20);
  Input *inputs = NULL;
  int n = parse_cases(data, len, &arena, &inputs);
  if (n < 0) tm_die("dump: parse failed\n");
  Buf ob; buf_init(&ob, &arena, 1 << 20);
  for (int ci = 0; ci < n; ci++) {
    Build b;
    build_case(&inputs[ci], &arena, &b);
    buf_puts(&ob, "CASE ");
    tm_put_int(&ob, ci);
    buf_putc(&ob, '\n');
    for (int i = 0; i < b.nsources; i++) {
      Source *s = &b.sources[i];
      buf_puts(&ob, "  ");
      buf_put_str(&ob, s->path);
      buf_puts(&ob, " post=");
      tm_put_int(&ob, s->post);
      buf_puts(&ob, " fm.tags=[");
      for (size_t t = 0; t < s->fm.ntags; t++) { if (t) buf_putc(&ob, '|'); buf_put_str(&ob, s->fm.tags[t]); Str sl = slugify(&arena, s->fm.tags[t]); buf_putc(&ob, '='); buf_put_str(&ob, sl); }
      buf_puts(&ob, "] refs=[");
      for (int t = 0; t < s->ntagRefs; t++) {
        if (t) buf_putc(&ob, '|');
        buf_put_str(&ob, s->tagRefs[t]->name);
        buf_putc(&ob, '/');
        buf_put_str(&ob, s->tagRefs[t]->slug);
      }
      buf_puts(&ob, "]\n");
    }
    buf_puts(&ob, "  GLOBAL tags=[");
    for (int t = 0; t < b.ntags; t++) {
      if (t) buf_putc(&ob, '|');
      buf_put_str(&ob, b.tags[t].name);
      buf_putc(&ob, '/');
      buf_put_str(&ob, b.tags[t].slug);
      buf_putc(&ob, '(');
      tm_put_int(&ob, b.tags[t].nposts);
      buf_putc(&ob, ')');
    }
    buf_puts(&ob, "]\n");
  }
  write_all(1, ob.buf, ob.len);
  return 0;
}


static int cmd_benchstages(int argc, char **argv) {
  if (argc < 4) tm_die("usage: engine_test benchstages <cases.json> <iters>\n");
  size_t len; uint8_t *data = map_file(argv[2], &len);
  if (!data) tm_die("benchstages: cannot map cases.json\n");
  long iters = tm_atoi(argv[3]);
  if (iters < 1) iters = 1;
  Arena oarena; arena_init(&oarena, 1 << 20);
  Buf ob; buf_init(&ob, &oarena, 4096);
  for (long it = 0; it < iters; it++) {
    Arena arena; arena_init(&arena, 256 << 20);
    int64_t t0 = tm_now_us();
    Input *inputs = NULL;
    int n = parse_cases(data, len, &arena, &inputs);
    int64_t t1 = tm_now_us();
    if (n < 0) tm_die("benchstages: parse failed\n");
    Buf out; buf_init(&out, &arena, 1 << 20);
    buf_putc(&out, '[');
    for (int i = 0; i < n; i++) {
      if (i) buf_putc(&out, ',');
      Build b;
      build_case(&inputs[i], &arena, &b);
      emit_case_json(&out, &b);
    }
    buf_putc(&out, ']');
    int64_t t2 = tm_now_us();
    uint64_t sum = 1469598103934665603ULL;
    for (size_t i = 0; i < out.len; i++) { sum ^= out.buf[i]; sum *= 1099511628211ULL; }
    buf_puts(&ob, "iter ");
    tm_put_int(&ob, it);
    buf_puts(&ob, ": parse=");
    tm_put_u64(&ob, (uint64_t)(t1 - t0));
    buf_puts(&ob, " build+emit=");
    tm_put_u64(&ob, (uint64_t)(t2 - t1));
    buf_puts(&ob, " total=");
    tm_put_u64(&ob, (uint64_t)(t2 - t0));
    buf_puts(&ob, " bytes=");
    tm_put_u64(&ob, (uint64_t)out.len);
    buf_puts(&ob, " sum=");
    tm_put_u64(&ob, sum);
    buf_putc(&ob, '\n');
    arena_free(&arena);
  }
  write_all(1, ob.buf, ob.len);
  return 0;
}


static int cmd_benchsteps(int argc, char **argv) {
  if (argc < 3) tm_die("usage: engine_test benchsteps <cases.json>\n");
  size_t len; uint8_t *data = map_file(argv[2], &len);
  if (!data) tm_die("benchsteps: cannot map cases.json\n");
  for (int i = 0; i < 16; i++) g_step_us[i] = 0;
  g_step_cur = -1;
  Arena arena; arena_init(&arena, 256 << 20);
  int64_t t0 = tm_now_us();
  Input *inputs = NULL;
  int n = parse_cases(data, len, &arena, &inputs);
  int64_t t1 = tm_now_us();
  if (n < 0) tm_die("benchsteps: parse failed\n");
  Buf out; buf_init(&out, &arena, 8 << 20);
  buf_putc(&out, '[');
  int64_t te0 = tm_now_us();
  for (int i = 0; i < n; i++) {
    if (i) buf_putc(&out, ',');
    Build b;
    build_case(&inputs[i], &arena, &b);
    int64_t e0 = tm_now_us();
    emit_case_json(&out, &b);
    g_step_us[10] += (uint64_t)(tm_now_us() - e0);
  }
  buf_putc(&out, ']');
  int64_t t2 = tm_now_us();
  static const char *names[11] = {"templates-parse", "sources", "frontmatter", "permalinks", "markdown", "collections", "tags", "values", "layouts", "report", "emit-json"};
  Arena oarena; arena_init(&oarena, 1 << 20);
  Buf ob; buf_init(&ob, &oarena, 4096);
  buf_puts(&ob, "parse_cases: ");
  tm_put_u64(&ob, (uint64_t)(t1 - t0));
  buf_puts(&ob, " us\n");
  uint64_t tot = 0;
  for (int i = 0; i < 11; i++) {
    buf_puts(&ob, names[i]);
    buf_puts(&ob, ": ");
    tm_put_u64(&ob, g_step_us[i]);
    buf_puts(&ob, " us\n");
    tot += g_step_us[i];
  }
  buf_puts(&ob, "memo hits: ");
  tm_put_u64(&ob, g_memo_hits);
  buf_puts(&ob, " miss(store): ");
  tm_put_u64(&ob, g_memo_miss);
  buf_puts(&ob, "\n");
  buf_puts(&ob, "build+emit steps total: ");
  tm_put_u64(&ob, tot);
  buf_puts(&ob, " us; wall total: ");
  tm_put_u64(&ob, (uint64_t)(t2 - t0));
  buf_puts(&ob, " us; emit-call overhead: ");
  tm_put_u64(&ob, (uint64_t)(te0 - t1));
  buf_puts(&ob, " us\n");
  write_all(1, ob.buf, ob.len);
  return 0;
}


static int cmd_benchjson(int argc, char **argv) {
  if (argc < 3) tm_die("usage: engine_test benchjson <file> [iters]\n");
  size_t len; uint8_t *data = map_file(argv[2], &len);
  if (!data) tm_die("benchjson: cannot map file\n");
  long iters = argc >= 4 ? tm_atoi(argv[3]) : 20;
  Arena arena; arena_init(&arena, 256 << 20);
  int64_t best = 1LL << 60, tot = 0;
  for (long it = 0; it < iters; it++) {
    arena_reset(&arena);
    Buf out; buf_init(&out, &arena, len + 1024);
    int64_t t0 = tm_now_us();
    json_emit_str(&out, (Str){ data, len });
    int64_t t1 = tm_now_us();
    if (t1 - t0 < best) best = t1 - t0;
    tot += t1 - t0;
    if (out.len < len + 2) tm_die("benchjson: length mismatch\n");
  }
  Arena oarena; arena_init(&oarena, 1 << 20);
  Buf ob; buf_init(&ob, &oarena, 256);
  buf_puts(&ob, "json_emit_str bytes=");
  tm_put_u64(&ob, (uint64_t)len);
  buf_puts(&ob, " best_us=");
  tm_put_u64(&ob, (uint64_t)best);
  buf_puts(&ob, " avg_us=");
  tm_put_u64(&ob, (uint64_t)(tot / iters));
  buf_puts(&ob, " MB/s best=");
  tm_put_u64(&ob, (uint64_t)(len * 1000000ULL / (uint64_t)(best > 0 ? best : 1) / 1000000ULL));
  buf_putc(&ob, '\n');
  write_all(1, ob.buf, ob.len);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { write_all(2, (const uint8_t *)"usage: engine_test <md|build|bench> ...\n", 37); return 1; }
  if (__builtin_strcmp(argv[1], "md") == 0) return cmd_md(argc, argv);
  if (__builtin_strcmp(argv[1], "build") == 0) return cmd_build(argc, argv);
  if (__builtin_strcmp(argv[1], "bench") == 0) return cmd_bench(argc, argv);
  if (__builtin_strcmp(argv[1], "dump") == 0) return cmd_dump(argc, argv);
  if (__builtin_strcmp(argv[1], "benchstages") == 0) return cmd_benchstages(argc, argv);
  if (__builtin_strcmp(argv[1], "benchsteps") == 0) return cmd_benchsteps(argc, argv);
  if (__builtin_strcmp(argv[1], "benchjson") == 0) return cmd_benchjson(argc, argv);
  write_all(2, (const uint8_t *)"unknown command\n", 16);
  return 1;
}
#endif
