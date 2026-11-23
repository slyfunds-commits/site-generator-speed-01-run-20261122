/* C markdown renderer — byte-exact port of solve.js renderMarkdown + inlineTo.
 *
 * Dual target: wasm32 (clang, SIMD128) and host x86-64 (gcc, SSE2) from the
 * SAME source. The host build exists so the engine can be differential-tested
 * natively under ASan/UBSan at full speed; the wasm build is what ships.
 *
 * Shape (per the world scan): freestanding, no libc, static .bss regions,
 * raw buffers only across the JS boundary. The two imports (linkFn, slugFn)
 * are the only JS calls, and both are per-heading/per-link (rare), never
 * per-byte. All markdown syntax bytes are ASCII, so multi-byte UTF-8
 * sequences pass through untouched.
 *
 * Memory regions (static .bss — no file bloat, demand-zero pages):
 *   IN      8 MiB  input body bytes (JS encodes here)
 *   JOIN    2 MiB  joined paragraph/quote/list scratch
 *   SCRATCH 64 KiB link import result: u32 len + bytes
 *   SCRATCH2 64 KiB slug import result: u32 len + bytes
 *   SLUG    1 KiB  current slug working buffer
 *   IDS     64 KiB dedup list: [u32 len][bytes] records
 *   OUT     4 MiB  html output
 *   META    64 KiB header + toc records
 * META header: [htmlLen u32][excerptOff u32][excerptLen u32][tocCount u32]
 * toc record (20 bytes): [level u32][idOff u32][idLen u32][htmlOff u32][htmlLen u32]
 * All META pointers are OUT-relative offsets (idOff/htmlOff/excerptOff are
 * added to out_ptr() by the harness) so host and wasm builds agree.
 */

#if defined(__wasm__)
#include <wasm_simd128.h>
typedef v128_t vec16;
#define MD_HAVE_SIMD 1
static inline vec16 v_load(const unsigned char *p) { return wasm_v128_load(p); }
static inline vec16 v_splat(unsigned char c) { return wasm_i8x16_splat((int8_t)c); }
static inline vec16 v_eq(vec16 a, vec16 b) { return wasm_i8x16_eq(a, b); }
static inline vec16 v_or(vec16 a, vec16 b) { return wasm_v128_or(a, b); }
static inline unsigned v_mask(vec16 a) { return (unsigned)wasm_i8x16_bitmask(a); }
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#include <immintrin.h>
typedef __m128i vec16;
#define MD_HAVE_SIMD 1
static inline vec16 v_load(const unsigned char *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline vec16 v_splat(unsigned char c) { return _mm_set1_epi8((char)c); }
static inline vec16 v_eq(vec16 a, vec16 b) { return _mm_cmpeq_epi8(a, b); }
static inline vec16 v_or(vec16 a, vec16 b) { return _mm_or_si128(a, b); }
static inline unsigned v_mask(vec16 a) { return (unsigned)_mm_movemask_epi8(a); }
/* AVX2 dispatch (round 8 Phase 2): 32-byte kernels with SSE2 fallback.
 * Test builds force a path with -DFORCE_AVX2 / -DFORCE_SSE2; otherwise
 * CPUID auto-detect (checks OS AVX support too). Never -march=native. */
static inline int md_use_avx2(void) {
#if defined(FORCE_AVX2)
  return 1;
#elif defined(FORCE_SSE2)
  return 0;
#else
  static int cached = -1;
  if (cached < 0) cached = __builtin_cpu_supports("avx2") ? 1 : 0;
  return cached;
#endif
}
#endif

typedef unsigned char u8;
typedef unsigned int u32;
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- regions ---- */
static u8 IN_BUF[8 * 1024 * 1024];
static u8 JOIN_BUF[2 * 1024 * 1024];
static u8 SCRATCH_BUF[64 * 1024];
static u8 SCRATCH2_BUF[64 * 1024];
static u8 SLUG_BUF[1024];
static u8 IDS_BUF[64 * 1024];
static u8 OUT_BUF[4 * 1024 * 1024];
static u8 META_BUF[64 * 1024];
static u8 TMP_BUF[8];

#define IN_CAP    (8 * 1024 * 1024)
#define JOIN_CAP  (2 * 1024 * 1024)
#define IDS_CAP   (64 * 1024)
#define OUT_CAP   (4 * 1024 * 1024)
#define META_CAP  (64 * 1024)

/* ---- imports (JS side; host harness provides stubs) ---- */
extern int linkFn(const u8 *p, int len);
extern int slugFn(const u8 *p, int len);

/* ---- state ---- */
static u8 *out_cur;
static u8 *join_cur;
static u8 *ids_cur;
static int toc_count;
static u32 excerpt_ptr;   /* offset from OUT_BUF */
static u32 excerpt_len;
static int excerpt_set;
static int overflow_flag;

/* ---- tiny memcpy (no libc; -fno-builtin keeps clang from synthesising calls) ----
 * 4-byte-at-a-time via byte-assembled u32 (portable, no unaligned UB —
 * the compiler emits a single unaligned load/store on x86/wasm). */
static void md_memcpy(u8 *d, const u8 *s, unsigned n) {
  while (n >= 4) {
    u32 v = (u32)s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)s[3] << 24);
    d[0] = (u8)v; d[1] = (u8)(v >> 8); d[2] = (u8)(v >> 16); d[3] = (u8)(v >> 24);
    d += 4; s += 4; n -= 4;
  }
  while (n--) *d++ = *s++;
}

/* Unaligned-safe u32 load/store (the region buffers are u8 arrays; a direct
 * *(u32 *) cast is UB and UBSan flags it. The compiler still emits a single
 * unaligned load/store on x86/wasm, which both allow. */
static u32 load_u32(const u8 *p) { u32 v; md_memcpy((u8 *)&v, p, 4); return v; }
static void store_u32(u8 *p, u32 v) { md_memcpy(p, (const u8 *)&v, 4); }

/* ---- output ---- */
static void out_byte(u8 b) {
  if (out_cur >= OUT_BUF + OUT_CAP) { overflow_flag = 1; return; }
  *out_cur++ = b;
}
static void out_mem(const u8 *p, int len) {
  if (len <= 0) return;
  if (out_cur + len > OUT_BUF + OUT_CAP) { overflow_flag = 1; return; }
  md_memcpy(out_cur, p, (unsigned)len);
  out_cur += len;
}
#define LIT(s) out_mem((const u8 *)(s), (int)(sizeof(s) - 1))

static void join_mem(const u8 *p, int len) {
  if (len <= 0) return;
  if (join_cur + len > JOIN_BUF + JOIN_CAP) { overflow_flag = 1; return; }
  md_memcpy(join_cur, p, (unsigned)len);
  join_cur += len;
}
static void join_byte(u8 b) {
  if (join_cur >= JOIN_BUF + JOIN_CAP) { overflow_flag = 1; return; }
  *join_cur++ = b;
}

/* ---- SIMD chunk scanners (simdjson stage-1 shape: 16-byte blocks, bitmask, ctz) ---- */

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static const u8 *find_byte_avx2(const u8 *p, const u8 *end, u8 c) {
  __m256i vc = _mm256_set1_epi8((char)c);
  while (end - p >= 32) {
    unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), vc));
    if (m) return p + __builtin_ctz(m);
    p += 32;
  }
  while (p < end) { if (*p == c) return p; p++; }
  return end;
}
#endif

/* First byte in [p,end) equal to c, or end. */
static const u8 *find_byte(const u8 *p, const u8 *end, u8 c) {
#if defined(__x86_64__) || defined(__i386__)
  if (md_use_avx2()) return find_byte_avx2(p, end, c);
#endif
#ifdef MD_HAVE_SIMD
  vec16 vc = v_splat(c);
  while (end - p >= 16) {
    unsigned m = v_mask(v_eq(v_load(p), vc));
    if (m) return p + __builtin_ctz(m);
    p += 16;
  }
#endif
  while (p < end) { if (*p == c) return p; p++; }
  return end;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static const u8 *find_esc_avx2(const u8 *p, const u8 *end) {
  __m256i am = _mm256_set1_epi8('&'), lt = _mm256_set1_epi8('<'), gt = _mm256_set1_epi8('>'),
          dq = _mm256_set1_epi8('"'), sq = _mm256_set1_epi8('\'');
  while (end - p >= 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __m256i m = _mm256_cmpeq_epi8(v, am);
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, lt));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, gt));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, dq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, sq));
    unsigned mask = (unsigned)_mm256_movemask_epi8(m);
    if (mask) return p + __builtin_ctz(mask);
    p += 32;
  }
  while (p < end) {
    u8 c = *p;
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return p;
    p++;
  }
  return end;
}
#endif

/* First escapable byte (& < > " ') in [p,end), or end. */
static const u8 *find_esc(const u8 *p, const u8 *end) {
#if defined(__x86_64__) || defined(__i386__)
  if (md_use_avx2()) return find_esc_avx2(p, end);
#endif
#ifdef MD_HAVE_SIMD
  vec16 am = v_splat('&'), lt = v_splat('<'), gt = v_splat('>'),
        dq = v_splat('"'), sq = v_splat('\'');
  while (end - p >= 16) {
    vec16 v = v_load(p);
    vec16 m = v_eq(v, am);
    m = v_or(m, v_eq(v, lt));
    m = v_or(m, v_eq(v, gt));
    m = v_or(m, v_eq(v, dq));
    m = v_or(m, v_eq(v, sq));
    unsigned mask = v_mask(m);
    if (mask) return p + __builtin_ctz(mask);
    p += 16;
  }
#endif
  while (p < end) {
    u8 c = *p;
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return p;
    p++;
  }
  return end;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static const u8 *find_special_avx2(const u8 *p, const u8 *end) {
  __m256i bs = _mm256_set1_epi8('\\'), bt = _mm256_set1_epi8('`'), st = _mm256_set1_epi8('*'), ob = _mm256_set1_epi8('['),
          am = _mm256_set1_epi8('&'), lt = _mm256_set1_epi8('<'), gt = _mm256_set1_epi8('>'),
          dq = _mm256_set1_epi8('"'), sq = _mm256_set1_epi8('\'');
  while (end - p >= 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __m256i m = _mm256_cmpeq_epi8(v, bs);
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, bt));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, st));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, ob));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, am));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, lt));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, gt));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, dq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, sq));
    unsigned mask = (unsigned)_mm256_movemask_epi8(m);
    if (mask) return p + __builtin_ctz(mask);
    p += 32;
  }
  while (p < end) {
    u8 c = *p;
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '&' ||
        c == '<' || c == '>' || c == '"' || c == '\'')
      return p;
    p++;
  }
  return end;
}
#endif

/* First byte in [p,end) in the inline-special set, or end.
 * Set (JS INLINE_SPECIAL): \ ` * [ & < > " ' */
static const u8 *find_special(const u8 *p, const u8 *end) {
#if defined(__x86_64__) || defined(__i386__)
  if (md_use_avx2()) return find_special_avx2(p, end);
#endif
#ifdef MD_HAVE_SIMD
  vec16 bs = v_splat('\\'), bt = v_splat('`'), st = v_splat('*'), ob = v_splat('['),
        am = v_splat('&'), lt = v_splat('<'), gt = v_splat('>'),
        dq = v_splat('"'), sq = v_splat('\'');
  while (end - p >= 16) {
    vec16 v = v_load(p);
    vec16 m = v_eq(v, bs);
    m = v_or(m, v_eq(v, bt));
    m = v_or(m, v_eq(v, st));
    m = v_or(m, v_eq(v, ob));
    m = v_or(m, v_eq(v, am));
    m = v_or(m, v_eq(v, lt));
    m = v_or(m, v_eq(v, gt));
    m = v_or(m, v_eq(v, dq));
    m = v_or(m, v_eq(v, sq));
    unsigned mask = v_mask(m);
    if (mask) return p + __builtin_ctz(mask);
    p += 16;
  }
#endif
  while (p < end) {
    u8 c = *p;
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '&' ||
        c == '<' || c == '>' || c == '"' || c == '\'')
      return p;
    p++;
  }
  return end;
}

/* HTML-escape bytes [p, p+len) into OUT. */
static void esc_push(const u8 *p, int len) {
  const u8 *end = p + len;
  const u8 *cur = p;
  while (cur < end) {
    const u8 *q = find_esc(cur, end);
    if (q > cur) out_mem(cur, (int)(q - cur));
    if (q >= end) break;
    switch (*q) {
      case '&':  LIT("&amp;"); break;
      case '<':  LIT("&lt;"); break;
      case '>':  LIT("&gt;"); break;
      case '"':  LIT("&quot;"); break;
      default:   LIT("&#39;"); break; /* ' */
    }
    cur = q + 1;
  }
}

/* Is the char at p (UTF-8) a JS \s whitespace char? Returns its byte length, 0 if not.
 * ASCII: 0x09-0x0D, 0x20. Unicode: U+00A0, U+1680, U+2000-200A, U+2028, U+2029,
 * U+202F, U+205F, U+3000, U+FEFF. */
static int space_len_at(const u8 *p, const u8 *end) {
  if (p >= end) return 0;
  u8 c = *p;
  if (c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32) return 1;
  if (c == 0xC2 && p + 1 < end && p[1] == 0xA0) return 2;
  if (c == 0xE1 && p + 2 < end && p[1] == 0x9A && p[2] == 0x80) return 3;
  if (c == 0xE2 && p + 2 < end && p[1] == 0x80) {
    u8 c2 = p[2];
    if ((c2 >= 0x80 && c2 <= 0x8A) || c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF) return 3;
  }
  if (c == 0xE2 && p + 2 < end && p[1] == 0x81 && p[2] == 0x9F) return 3;
  if (c == 0xE3 && p + 2 < end && p[1] == 0x80 && p[2] == 0x80) return 3;
  if (c == 0xEF && p + 2 < end && p[1] == 0xBB && p[2] == 0xBF) return 3;
  return 0;
}

/* ---- slug ---- */

/* ASCII slug fast path over bytes [p, p+len). Returns length in SLUG_BUF, or -1 if non-ASCII.
 * Mirrors slugifyUncached: lowercase A-Z, keep a-z0-9, runs of others -> one '-',
 * trim trailing '-', slice to 64, 'untitled' when empty. (Building the full slug
 * then slicing is equivalent to capping at 64 during the build: the first 64
 * chars are identical either way, and a '-' at the cut is trimmed in both.) */
static int slug_ascii(const u8 *p, int len) {
  int o = 0;
  int gap = 0;
  for (int i = 0; i < len; i++) {
    u8 c = p[i];
    if (c > 127) return -1;
    int keep = -1;
    if (c >= 65 && c <= 90) keep = c + 32;
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep < 0) {
      if (o > 0) gap = 1;
      continue;
    }
    if (o >= 64) break; /* output full; JS slice(0,64) drops the rest */
    if (gap) {
      SLUG_BUF[o++] = 45;
      gap = 0;
      if (o >= 64) break;
    }
    SLUG_BUF[o++] = (u8)keep;
  }
  while (o > 0 && SLUG_BUF[o - 1] == 45) o--;
  if (o == 0) {
    static const char unt[] = "untitled";
    md_memcpy(SLUG_BUF, (const u8 *)unt, 8);
    return 8;
  }
  return o;
}

static int base_len; /* base slug length before any "-n" suffix */

static int append_suffix(int n) {
  /* SLUG_BUF holds base[0..base_len); rebuild "-n" after it. */
  int o = base_len;
  SLUG_BUF[o++] = 45;
  int tmp = n, digits = 0, rev = 0;
  while (tmp > 0) { rev = rev * 10 + (tmp % 10); tmp /= 10; digits++; }
  if (digits == 0) { SLUG_BUF[o++] = 48; }
  else {
    for (int i = 0; i < digits; i++) { SLUG_BUF[o++] = (u8)(48 + (rev % 10)); rev /= 10; }
  }
  return o;
}

/* uniqueSlug: dedupe against IDS list; writes final id bytes to SLUG_BUF, returns len. */
static int unique_slug(const u8 *p, int len) {
  int slen = slug_ascii(p, len);
  if (slen < 0) {
    /* non-ASCII: ask JS (NFKD path). slugFn writes u32 len + bytes at SCRATCH2. */
    int n = slugFn(p, len);
    if (n <= 0) {
      static const char unt[] = "untitled";
      md_memcpy(SLUG_BUF, (const u8 *)unt, 8);
      slen = 8;
    } else {
      u32 got = load_u32(SCRATCH2_BUF);
      md_memcpy(SLUG_BUF, SCRATCH2_BUF + 4, got);
      slen = (int)got;
    }
  }
  base_len = slen; /* base length before any "-n" suffix */
  int n = 2;
  while (1) {
    int found = 0;
    u8 *q = IDS_BUF;
    while (q < ids_cur) {
      u32 l = load_u32(q);
      if ((int)l == slen) {
        int eq = 1;
        for (int i = 0; i < slen; i++) {
          if (q[4 + i] != SLUG_BUF[i]) { eq = 0; break; }
        }
        if (eq) { found = 1; break; }
      }
      q += 4 + l;
    }
    if (!found) break;
    slen = append_suffix(n);
    n++;
  }
  if (ids_cur + 4 + slen <= IDS_BUF + IDS_CAP) {
    store_u32(ids_cur, (u32)slen);
    md_memcpy(ids_cur + 4, SLUG_BUF, (unsigned)slen);
    ids_cur += 4 + slen;
  }
  return slen;
}

/* ---- line classification ---- */

/* End index (excl '\n') of the line starting at p; N if no '\n'. */
static const u8 *line_end(const u8 *p, const u8 *N) {
  return find_byte(p, N, 10);
}

static int is_blank_range(const u8 *a, const u8 *b) {
  for (const u8 *k = a; k < b; k++) { if (*k != 32 && *k != 9) return 0; }
  return 1;
}

/* fence open: returns lang length (>=0) or -1. Lang ptr in fence_lang_ptr. */
static const u8 *fence_lang_ptr;
static int fence_open_range(const u8 *a, const u8 *b) {
  if (b - a < 3) return -1;
  if (a[0] != 96 || a[1] != 96 || a[2] != 96) return -1;
  const u8 *j = a + 3;
  while (j < b) {
    u8 c = *j;
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) ||
        c == 95 || c == 43 || c == 45) j++;
    else break;
  }
  fence_lang_ptr = a + 3;
  int lang_len = (int)(j - (a + 3));
  while (j < b && (*j == 32 || *j == 9)) j++;
  return j == b ? lang_len : -1;
}

static int is_fence_close_range(const u8 *a, const u8 *b) {
  if (b - a < 3) return 0;
  if (a[0] != 96 || a[1] != 96 || a[2] != 96) return 0;
  for (const u8 *k = a + 3; k < b; k++) { if (*k != 32 && *k != 9) return 0; }
  return 1;
}

static int head_depth;
static const u8 *head_text_ptr;
static int head_text_len;
static int heading_range(const u8 *a, const u8 *b) {
  int d = 0;
  while (a + d < b && a[d] == 35) d++;
  if (d == 0 || d > 6 || a + d >= b) return 0;
  u8 c = a[d];
  if (c != 32 && c != 9) return 0;
  const u8 *st = a + d;
  while (st < b && (*st == 32 || *st == 9)) st++;
  const u8 *e = b;
  while (e > st && (e[-1] == 32 || e[-1] == 9)) e--;
  if (e <= st) return 0;
  head_depth = d;
  head_text_ptr = st;
  head_text_len = (int)(e - st);
  return 1;
}

static int is_hr_range(const u8 *a, const u8 *b) {
  if (b - a < 3) return 0;
  u8 c = a[0];
  if (c != 45 && c != 42) return 0;
  if (a[1] != c || a[2] != c) return 0;
  for (const u8 *k = a + 3; k < b; k++) { if (*k != 32 && *k != 9) return 0; }
  return 1;
}

static int list_ordered;
static int list_start;
static int list_start_range(const u8 *a, const u8 *b) {
  u8 c0 = a[0];
  if (c0 == 45 || c0 == 42) {
    if (b - a >= 2 && a[1] == 32) { list_ordered = 0; list_start = 1; return 1; }
    return 0;
  }
  if (c0 >= 48 && c0 <= 57) {
    const u8 *j = a;
    while (j < b && *j >= 48 && *j <= 57) j++;
    if (j - a >= 1 && j - a <= 9 && j + 1 < b && *j == 46 && j[1] == 32) {
      int v = 0;
      for (const u8 *k = a; k < j; k++) v = v * 10 + (*k - 48);
      list_ordered = 1;
      list_start = v;
      return 1;
    }
  }
  return 0;
}

/* trim [a,b) of spaces/tabs; writes [a,b) into TMP_BUF as two u32 offsets from IN_BUF. */
static void trim_range(const u8 *a, const u8 *b) {
  while (a < b && (*a == 32 || *a == 9)) a++;
  while (b > a && (b[-1] == 32 || b[-1] == 9)) b--;
  store_u32(TMP_BUF, (u32)(a - IN_BUF));
  store_u32(TMP_BUF + 4, (u32)(b - IN_BUF));
}

/* classify: 1 blank, 2 fence, 3 heading, 4 hr, 5 quote, 6 list, 7 other.
 * Side effects set the fence_lang_ptr, head_* and list_* globals. */
static int classify(const u8 *a, const u8 *b) {
  if (a == b) return 1;
  int blank = 1;
  for (const u8 *k = a; k < b; k++) { if (*k != 32 && *k != 9) { blank = 0; break; } }
  if (blank) return 1;
  u8 c0 = a[0];
  if (c0 == 96) {
    if (fence_open_range(a, b) >= 0) return 2;
  } else if (c0 == 35) {
    if (heading_range(a, b)) return 3;
  } else if (c0 == 45 || c0 == 42) {
    if (is_hr_range(a, b)) return 4;
  } else if (c0 == 62) {
    return 5;
  }
  if (list_start_range(a, b)) return 6;
  return 7;
}

/* CONTINUED = /^ {2,}\S/ on line [a,b) */
static int is_continued(const u8 *a, const u8 *b) {
  if (b - a < 2) return 0;
  if (a[0] != 32 || a[1] != 32) return 0;
  const u8 *k = a;
  while (k < b && *k == 32) k++;
  if (k >= b) return 0;
  return space_len_at(k, b) == 0;
}

/* ---- inline helpers ---- */

static int escaped_before(const u8 *pos, const u8 *from) {
  int n = 0;
  for (const u8 *k = pos - 1; k >= from && *k == 92; k--) n++;
  return (n & 1) == 1;
}

static int is_escapable(u8 c) {
  return c == 92 || c == 96 || c == 42 || c == 95 || c == 91 || c == 93 || c == 40 || c == 41 ||
         c == 35 || c == 43 || c == 45 || c == 46 || c == 33 || c == 62;
}

/* find next occurrence of `**` at-or-after from, fully inside [from,to), not escaped; NULL if none. */
static const u8 *find_unescaped_starstar(const u8 *from, const u8 *to) {
  const u8 *i = from;
  while (i + 1 < to) {
    if (i[0] == 42 && i[1] == 42) {
      if (i + 2 > to) return NULL;
      if (!escaped_before(i, from)) return i;
      i++;
    } else {
      i++;
    }
  }
  return NULL;
}

/* find next ']' at-or-after from, inside [from,to), not escaped; NULL if none. */
static const u8 *find_unescaped_bracket(const u8 *from, const u8 *to) {
  const u8 *i = from;
  while (i < to) {
    if (*i == 93) {
      if (!escaped_before(i, from)) return i;
      i++;
    } else {
      i++;
    }
  }
  return NULL;
}

/* find first single * (see JS findSingleStar for exact semantics). Returns the
 * position of the single star (the run's last star), or NULL. */
static const u8 *find_single_star(const u8 *from, const u8 *to) {
  const u8 *i = from;
  while (i < to) {
    if (*i != 42) { i++; continue; }
    const u8 *start = i;
    const u8 *end = i;
    while (end < to && *end == 42) end++;
    if (start >= to) return NULL;
    int odd = (((end - start) & 1) == 1);
    if (odd != escaped_before(start, from)) return end - 1;
    i = end;
  }
  return NULL;
}

/* find next ')' in [from, to); NULL if none. */
static const u8 *find_close_paren(const u8 *from, const u8 *to) {
  const u8 *r = find_byte(from, to, 41);
  return r == to ? NULL : r;
}

/* does [p, p+len) contain JS \s whitespace? */
static int has_ws(const u8 *p, int len) {
  const u8 *end = p + len;
  const u8 *i = p;
  while (i < end) {
    int n = space_len_at(i, end);
    if (n > 0) return 1;
    i++;
  }
  return 0;
}

/* inline render of bytes [from, to) into OUT. Recursive for strong/em/link interiors. */
static void inline_to(const u8 *from, const u8 *to) {
  const u8 *i = from;
  const u8 *run = from;
  const u8 *n = to;
  while (1) {
    i = find_special(i, n);
    if (i >= n) break;
    u8 code = *i;
    if (code == 92) {
      /* backslash */
      if (i + 1 < n && is_escapable(i[1])) {
        if (run < i) out_mem(run, (int)(i - run));
        u8 e = i[1];
        if (e == 38) LIT("&amp;");
        else if (e == 60) LIT("&lt;");
        else if (e == 62) LIT("&gt;");
        else if (e == 34) LIT("&quot;");
        else if (e == 39) LIT("&#39;");
        else out_byte(e);
        i += 2;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code == 96) {
      /* backtick: JS indexOf('`', i+1) over the whole string, requires j < n */
      const u8 *j = find_byte(i + 1, n, 96);
      if (j < n && j > i + 1) {
        if (run < i) out_mem(run, (int)(i - run));
        LIT("<code>");
        esc_push(i + 1, (int)(j - (i + 1)));
        LIT("</code>");
        i = j + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code == 42) {
      if (i + 1 < n && i[1] == 42) {
        const u8 *j = find_unescaped_starstar(i + 2, n);
        if (j && j > i + 2) {
          if (run < i) out_mem(run, (int)(i - run));
          LIT("<strong>");
          inline_to(i + 2, j);
          LIT("</strong>");
          i = j + 2;
          run = i;
        } else {
          i += 2;
        }
        continue;
      }
      const u8 *j = find_single_star(i + 1, n);
      if (j && j > i + 1) {
        if (run < i) out_mem(run, (int)(i - run));
        LIT("<em>");
        inline_to(i + 1, j);
        LIT("</em>");
        i = j + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code == 91) {
      /* [ */
      const u8 *k = find_unescaped_bracket(i + 1, n);
      const u8 *m2 = NULL;
      if (k && k + 1 < n && k[1] == 40) m2 = find_close_paren(k + 2, n);
      const u8 *target_ptr = NULL;
      int target_len = 0;
      if (m2) { target_ptr = k + 2; target_len = (int)(m2 - (k + 2)); }
      if (target_len > 0 && !has_ws(target_ptr, target_len)) {
        if (run < i) out_mem(run, (int)(i - run));
        LIT("<a href=\"");
        int r = linkFn(target_ptr, target_len);
        if (r == 1) {
          u32 l = load_u32(SCRATCH_BUF);
          esc_push(SCRATCH_BUF + 4, (int)l);
        } else {
          esc_push(target_ptr, target_len);
        }
        LIT("\">");
        inline_to(i + 1, k);
        LIT("</a>");
        i = m2 + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    /* & < > " ' */
    if (run < i) out_mem(run, (int)(i - run));
    if (code == 38) LIT("&amp;");
    else if (code == 60) LIT("&lt;");
    else if (code == 62) LIT("&gt;");
    else if (code == 34) LIT("&quot;");
    else LIT("&#39;");
    i++;
    run = i;
  }
  if (run < n) out_mem(run, (int)(n - run));
}

/* ---- block renderer ---- */

/* Record a toc entry. id_off/html_off are offsets from OUT_BUF (NOT absolute
 * addresses) so host and wasm builds agree; the harness adds out_ptr(). */
static void toc_push(int level, u32 id_off, int id_len, u32 html_off, int html_len) {
  u8 *rec = META_BUF + 16 + toc_count * 20;
  if (rec + 20 > META_BUF + META_CAP) { overflow_flag = 1; return; }
  store_u32(rec + 0, (u32)level);
  store_u32(rec + 4, id_off);
  store_u32(rec + 8, (u32)id_len);
  store_u32(rec + 12, html_off);
  store_u32(rec + 16, (u32)html_len);
  toc_count++;
}

/* Render body at IN_BUF[0..in_len). Returns html length, or -1 on overflow. */
#ifdef __wasm__
#define EXPORT(n) __attribute__((export_name(#n)))
#else
#define EXPORT(n)
#endif

EXPORT(in_ptr) u8 *in_ptr(void) { return IN_BUF; }
EXPORT(out_ptr) u8 *out_ptr(void) { return OUT_BUF; }
EXPORT(meta_ptr) u8 *meta_ptr(void) { return META_BUF; }
EXPORT(scratch_ptr) u8 *scratch_ptr(void) { return SCRATCH_BUF; }
EXPORT(scratch2_ptr) u8 *scratch2_ptr(void) { return SCRATCH2_BUF; }

EXPORT(render_md)
int render_md(int in_len) {
  out_cur = OUT_BUF;
  join_cur = JOIN_BUF;
  ids_cur = IDS_BUF;
  toc_count = 0;
  excerpt_ptr = 0;
  excerpt_len = 0;
  excerpt_set = 0;
  overflow_flag = 0;
  const u8 *N = IN_BUF + in_len;
  const u8 *pos = IN_BUF;
  while (pos <= N) {
    const u8 *a = pos;
    const u8 *b = line_end(pos, N);
    const u8 *next_pos = b + 1;
    int k = classify(a, b);
    if (k == 1) { pos = next_pos; if (b == N) break; continue; }
    if (k == 2) {
      /* fence */
      int lang_len = fence_open_range(a, b);
      const u8 *lang_ptr = fence_lang_ptr;
      LIT("<pre><code");
      if (lang_len > 0) { LIT(" class=\"language-"); esc_push(lang_ptr, lang_len); LIT("\""); }
      LIT(">");
      const u8 *body_start = next_pos;
      const u8 *body_end = N;
      int closed = 0;
      const u8 *p2 = next_pos;
      while (p2 <= N) {
        const u8 *a2 = p2;
        const u8 *b2 = line_end(p2, N);
        if (b2 > a2 && is_fence_close_range(a2, b2)) { body_end = a2; closed = 1; p2 = b2 + 1; break; }
        if (b2 == N) { body_end = N; p2 = N + 1; break; }
        p2 = b2 + 1;
      }
      if (closed) {
        if (body_end > body_start) esc_push(body_start, (int)(body_end - body_start));
      } else if (b < N) {
        esc_push(body_start, (int)(N - body_start));
        out_byte(10);
      }
      LIT("</code></pre>\n");
      pos = closed ? p2 : N + 1;
      if (pos > N) break;
      continue;
    }
    if (k == 3) {
      /* heading */
      int depth = head_depth;
      const u8 *t_ptr = head_text_ptr;
      int t_len = head_text_len;
      int slen = unique_slug(t_ptr, t_len);
      LIT("<h");
      out_byte((u8)(48 + depth));
      LIT(" id=\"");
      out_mem(SLUG_BUF, slen);
      LIT("\">");
      u32 html_start = (u32)(out_cur - OUT_BUF);
      inline_to(t_ptr, t_ptr + t_len);
      int html_len = (int)((out_cur - OUT_BUF) - html_start);
      LIT("</h");
      out_byte((u8)(48 + depth));
      LIT(">\n");
      if (depth == 2 || depth == 3) {
        /* OUT layout: <hN id=" ID "> HTML </hN>. id starts at html_start - slen - 2. */
        toc_push(depth, html_start - (u32)slen - 2, slen, html_start, html_len);
      }
      pos = next_pos;
      if (b == N) break;
      continue;
    }
    if (k == 4) {
      LIT("<hr>\n");
      pos = next_pos;
      if (b == N) break;
      continue;
    }
    if (k == 5) {
      /* quote */
      const u8 *join_start = join_cur;
      int kept = 0;
      const u8 *p2 = pos;
      while (p2 <= N) {
        const u8 *a2 = p2;
        const u8 *b2 = line_end(p2, N);
        if (!(b2 > a2 && *a2 == 62)) break;
        const u8 *st = (a2 + 1 < b2 && a2[1] == 32) ? a2 + 2 : a2 + 1;
        trim_range(st, b2);
        const u8 *ta = IN_BUF + load_u32(TMP_BUF);
        const u8 *tb = IN_BUF + load_u32(TMP_BUF + 4);
        if (ta < tb) {
          if (kept > 0) join_byte(10);
          join_mem(ta, (int)(tb - ta));
          kept++;
        }
        if (b2 == N) { p2 = N + 1; break; }
        p2 = b2 + 1;
      }
      if (kept == 0) {
        LIT("<blockquote>\n</blockquote>\n");
      } else {
        LIT("<blockquote>\n<p>");
        inline_to(join_start, join_cur);
        LIT("</p>\n</blockquote>\n");
      }
      pos = p2;
      if (p2 > N) break;
      continue;
    }
    if (k == 6) {
      /* list — single pass: open tag first (start is known from line 1),
       * then render each item when the next one begins. */
      int is_ordered = list_ordered;
      int first_start = list_start;
      if (is_ordered && first_start != 1) {
        LIT("<ol start=\"");
        int v = first_start;
        if (v == 0) { out_byte(48); }
        else {
          int digits = 0, rev = 0;
          while (v > 0) { rev = rev * 10 + (v % 10); v /= 10; digits++; }
          for (int i = 0; i < digits; i++) { out_byte((u8)(48 + (rev % 10))); rev /= 10; }
        }
        LIT("\">\n");
      } else if (is_ordered) {
        LIT("<ol>\n");
      } else {
        LIT("<ul>\n");
      }
      const u8 *tstart;
      if (!is_ordered) tstart = a + 2;
      else { const u8 *j = a; while (j < b && *j >= 48 && *j <= 57) j++; tstart = j + 2; }
      const u8 *item_start = join_cur;
      trim_range(tstart, b);
      join_mem(IN_BUF + load_u32(TMP_BUF), (int)(load_u32(TMP_BUF + 4) - load_u32(TMP_BUF)));
      const u8 *p2 = next_pos;
      while (p2 <= N) {
        const u8 *a2 = p2;
        const u8 *b2 = line_end(p2, N);
        if (b2 == N && a2 == N) break;
        int k2 = classify(a2, b2);
        if (k2 == 6 && list_ordered == is_ordered) {
          LIT("<li>");
          inline_to(item_start, join_cur);
          LIT("</li>\n");
          const u8 *ts;
          if (!is_ordered) ts = a2 + 2;
          else { const u8 *j = a2; while (j < b2 && *j >= 48 && *j <= 57) j++; ts = j + 2; }
          item_start = join_cur;
          trim_range(ts, b2);
          join_mem(IN_BUF + load_u32(TMP_BUF), (int)(load_u32(TMP_BUF + 4) - load_u32(TMP_BUF)));
          if (b2 == N) { p2 = N + 1; break; }
          p2 = b2 + 1;
        } else if (b2 - a2 >= 2 && a2[0] == 32 && a2[1] == 32 && is_continued(a2, b2)) {
          trim_range(a2, b2);
          const u8 *ta = IN_BUF + load_u32(TMP_BUF);
          const u8 *tb = IN_BUF + load_u32(TMP_BUF + 4);
          join_byte(10);
          join_mem(ta, (int)(tb - ta));
          if (b2 == N) { p2 = N + 1; break; }
          p2 = b2 + 1;
        } else break;
      }
      LIT("<li>");
      inline_to(item_start, join_cur);
      LIT("</li>\n");
      if (is_ordered) LIT("</ol>\n"); else LIT("</ul>\n");
      pos = p2;
      if (p2 > N) break;
      continue;
    }
    /* paragraph */
    {
      const u8 *join_start = join_cur;
      trim_range(a, b);
      join_mem(IN_BUF + load_u32(TMP_BUF), (int)(load_u32(TMP_BUF + 4) - load_u32(TMP_BUF)));
      const u8 *p2 = next_pos;
      while (p2 <= N) {
        const u8 *a2 = p2;
        const u8 *b2 = line_end(p2, N);
        if (b2 == N && a2 == N) break;
        int k2 = classify(a2, b2);
        if (k2 == 1 || k2 == 2 || k2 == 3 || k2 == 4) break;
        trim_range(a2, b2);
        join_byte(10);
        join_mem(IN_BUF + load_u32(TMP_BUF), (int)(load_u32(TMP_BUF + 4) - load_u32(TMP_BUF)));
        if (b2 == N) { p2 = N + 1; break; }
        p2 = b2 + 1;
      }
      LIT("<p>");
      if (!excerpt_set) {
        u32 html_start = (u32)(out_cur - OUT_BUF);
        inline_to(join_start, join_cur);
        excerpt_ptr = html_start;
        excerpt_len = (u32)((out_cur - OUT_BUF) - html_start);
        excerpt_set = 1;
      } else {
        inline_to(join_start, join_cur);
      }
      LIT("</p>\n");
      pos = p2;
      if (p2 > N) break;
    }
  }
  int html_len = (int)(out_cur - OUT_BUF);
  store_u32(META_BUF + 0, (u32)html_len);
  store_u32(META_BUF + 4, excerpt_ptr);
  store_u32(META_BUF + 8, excerpt_len);
  store_u32(META_BUF + 12, (u32)toc_count);
  if (overflow_flag) return -1;
  return html_len;
}
