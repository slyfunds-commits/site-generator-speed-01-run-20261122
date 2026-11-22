// site-generator-speed-01-run-20261122: a fast drop-in for makefun-site 1.0.0.
//
// Hand-written, single-pass, dependency-free. It reproduces makefun-site's
// output byte for byte (front matter, template dialect, Markdown subset, slug
// rule, report) without calling the reference and without a token tree or
// AST: blocks render straight to output buffers, inline rendering scans
// left to right with first-character dispatch, and every regex is compiled
// lazily or replaced by a small hand scanner. No WebAssembly, no native code,
// no binary, no network, no clock, no cross-case state.

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

// ---------------------------------------------------------------- text

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const ESC_TEST = /[&<>"']/;
const ESC_REPLACE = /[&<>"']/g;

/** `text` with & < > " and ' escaped. */
export function escapeHtml(text) {
  if (!ESC_TEST.test(text)) return text;
  return text.replace(ESC_REPLACE, (c) => ESCAPES[c]);
}

/** Push escapeHtml(text) into `out` without building the string. */
function escPush(text, out) {
  out.push(text.replace(ESC_REPLACE, (c) => ESCAPES[c]));
}

export const SLUG_MAX = 64;

/** Per-case slug cache (a pure function; rebuilt for every case). */
let slugCache = null;

/**
 * The slug of `text`: NFKD, combining marks dropped, A-Z lowered, every run
 * of characters other than a-z and 0-9 one "-", none at either end, at most
 * SLUG_MAX characters (a "-" left at the cut dropped), "untitled" when
 * nothing is left. ASCII text takes a fast path (NFKD is identity there);
 * anything else follows the reference's own steps exactly.
 */
export function slugify(text) {
  if (slugCache !== null) {
    const hit = slugCache.get(text);
    if (hit !== undefined) return hit;
  }
  const out = slugifyUncached(text);
  if (slugCache !== null) slugCache.set(text, out);
  return out;
}

function slugifyUncached(text) {
  let ascii = true;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    let out = '';
    let gap = false;
    for (let i = 0; i < text.length; i++) {
      const ch = text[i];
      let keep = null;
      if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
      else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
      if (keep === null) {
        gap = out.length > 0;
        continue;
      }
      if (gap) out += '-';
      gap = false;
      out += keep;
    }
    out = out.slice(0, SLUG_MAX);
    let e = out.length;
    while (e > 0 && out.charCodeAt(e - 1) === 45) e--;
    out = out.slice(0, e);
    return out === '' ? 'untitled' : out;
  }
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, SLUG_MAX).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

/** `slug` made unique against `taken` (and added to it). */
export function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/** A YYYY-MM-DD date in words, from the string alone. */
export function longDate(date) {
  return `${Number(date.slice(8, 10))} ${MONTHS[Number(date.slice(5, 7)) - 1]} ${date.slice(0, 4)}`;
}

// ---------------------------------------------------------------- front matter

const KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const LINE = /^([a-z_]+):(?: (.*))?$/;
const DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const NAME = /^[a-z0-9-]+$/;
const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const TAG = /^[^"[\]]+$/;
const BLANK_RE = /^[ \t]*$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

/** A string value: "..." with \" and \\ its only escapes, or a bare string. */
function stringOf(raw) {
  if (raw.startsWith('"')) {
    if (raw.length < 2 || !raw.endsWith('"')) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.startsWith('[')) return { error: 'a bare string opens on [' };
  return { value: raw };
}

/** One key's value, or why it is refused. */
function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = DATE.exec(raw);
      const y = m ? Number(m[1]) : 0;
      const mo = m ? Number(m[2]) : 0;
      const d = m ? Number(m[3]) : 0;
      if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (!raw.startsWith('[') || !raw.endsWith(']')) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',').map((t) => t.trim());
      if (tags.some((t) => t === '' || !TAG.test(t))) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return SLUG.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

/**
 * A source's front matter and body: { data, body }, or { error }. `text`
 * has "\n" line ends. Only the front-matter lines are split out; the body
 * is the untouched rest of the text.
 */
export function parseFrontMatter(text) {
  const nl = text.indexOf('\n');
  const first = nl === -1 ? text : text.slice(0, nl);
  if (first !== '---') return { error: 'no front matter' };
  let cur = nl === -1 ? text.length : nl + 1;
  let closeStart = -1;
  let closeEnd = -1;
  while (cur <= text.length) {
    const eol = text.indexOf('\n', cur);
    const end = eol === -1 ? text.length : eol;
    if (text.slice(cur, end) === '---') {
      closeStart = cur;
      closeEnd = end;
      break;
    }
    if (eol === -1) break;
    cur = end + 1;
  }
  if (closeStart === -1) return { error: 'the front matter is not closed' };
  let fm = text.slice(nl + 1, closeStart);
  if (fm.charCodeAt(fm.length - 1) === 10) fm = fm.slice(0, -1);
  const fmLines = fm === '' ? [] : fm.split('\n');
  const data = {};
  for (let idx = 0; idx < fmLines.length; idx++) {
    const line = fmLines[idx];
    if (BLANK_RE.test(line) || line.startsWith('#')) continue;
    const m = LINE.exec(line);
    if (!m) return { error: `line ${idx + 2} is not "key: value"` };
    const key = m[1];
    const raw = (m[2] ?? '').replace(/[ \t]+$/, '');
    if (!KEYS.includes(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = valueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  return { data, body: closeEnd < text.length ? text.slice(closeEnd + 1) : '' };
}

// ---------------------------------------------------------------- markdown

const ESCAPABLE = new Set(['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']);
const WS_RE = /\s/;

/** Whether `line` holds only spaces and tabs (or is empty). */
function isBlank(line) {
  for (let k = 0; k < line.length; k++) {
    const c = line.charCodeAt(k);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** A fence-open line's language, or null. */
function fenceOpen(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return null;
  let j = 3;
  while (j < line.length) {
    const c = line.charCodeAt(j);
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) j++;
    else break;
  }
  const lang = line.slice(3, j);
  while (j < line.length && (line.charCodeAt(j) === 32 || line.charCodeAt(j) === 9)) j++;
  return j === line.length ? lang : null;
}

/** Whether `line` closes a fence. */
function isFenceClose(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return false;
  for (let k = 3; k < line.length; k++) {
    const c = line.charCodeAt(k);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** A heading line's level and text, or null. */
function headingOf(line) {
  let d = 0;
  while (d < line.length && line.charCodeAt(d) === 35) d++;
  if (d === 0 || d > 6 || d >= line.length) return null;
  const c = line.charCodeAt(d);
  if (c !== 32 && c !== 9) return null;
  let s = d;
  while (s < line.length && (line.charCodeAt(s) === 32 || line.charCodeAt(s) === 9)) s++;
  let e = line.length;
  while (e > s && (line.charCodeAt(e - 1) === 32 || line.charCodeAt(e - 1) === 9)) e--;
  if (e <= s) return null;
  return { depth: d, text: line.slice(s, e) };
}

/** Whether `line` is a thematic break. */
function isHR(line) {
  const c = line.charCodeAt(0);
  if (c !== 45 && c !== 42) return false;
  if (line.charCodeAt(1) !== c || line.charCodeAt(2) !== c) return false;
  for (let k = 3; k < line.length; k++) {
    const cc = line.charCodeAt(k);
    if (cc !== 32 && cc !== 9) return false;
  }
  return true;
}

/** Whether `line` ends a paragraph: blank, fence, heading or break. */
function endsParagraph(line) {
  if (line.length === 0) return true;
  const c0 = line.charCodeAt(0);
  if (c0 === 32 || c0 === 9) return isBlank(line);
  if (c0 === 96) return fenceOpen(line) !== null;
  if (c0 === 35) return headingOf(line) !== null;
  if (c0 === 45 || c0 === 42) return isHR(line);
  return false;
}

/** A paragraph line as rendered: spaces and tabs at either end dropped. */
function trimLine(line) {
  let a = 0;
  let b = line.length;
  while (a < b && (line.charCodeAt(a) === 32 || line.charCodeAt(a) === 9)) a++;
  while (b > a && (line.charCodeAt(b - 1) === 32 || line.charCodeAt(b - 1) === 9)) b--;
  return line.slice(a, b);
}

/**
 * A list's first line, or null: { ordered, start, text }. A bullet list
 * opens on "- " or "* "; an ordered list on 1-9 digits and ". ".
 */
function listStart(line) {
  const c0 = line.charCodeAt(0);
  if (c0 === 45 || c0 === 42) {
    if (line.charCodeAt(1) === 32) return { ordered: false, start: 1, text: line.slice(2) };
    return null;
  }
  if (c0 >= 48 && c0 <= 57) {
    let j = 0;
    while (j < line.length && line.charCodeAt(j) >= 48 && line.charCodeAt(j) <= 57) j++;
    if (j >= 1 && j <= 9 && line.charCodeAt(j) === 46 && line.charCodeAt(j + 1) === 32) {
      return { ordered: true, start: Number(line.slice(0, j)), text: line.slice(j + 2) };
    }
  }
  return null;
}

const CONTINUED = /^ {2,}\S/;

/** The first index from `from` where `s` holds `what` outside an escape. */
function findUnescaped(s, what, from) {
  for (let j = from; j <= s.length - what.length; j++) {
    if (s[j] === '\\' && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) {
      j++;
      continue;
    }
    if (s.startsWith(what, j)) return j;
  }
  return -1;
}

/** The first single * from `from` (one whose run is not **), outside an escape. */
function findSingleStar(s, from) {
  for (let j = from; j < s.length; j++) {
    const c = s[j];
    if (c === '\\' && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) {
      j++;
      continue;
    }
    if (c !== '*') continue;
    if (s[j + 1] === '*') {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}

/** `s` rendered inline into `out`; `env.link(target)` is a link's href. */
export function inlineTo(s, env, out) {
  let i = 0;
  const n = s.length;
  let run = 0;
  while (i < n) {
    const code = s.charCodeAt(i);
    if (code === 92) {
      // backslash
      if (i + 1 < n && ESCAPABLE.has(s[i + 1])) {
        if (run < i) out.push(s.slice(run, i));
        const e = ESCAPES[s[i + 1]];
        out.push(e === undefined ? s[i + 1] : e);
        i += 2;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code === 96) {
      // backtick
      const j = s.indexOf('`', i + 1);
      if (j > i + 1) {
        if (run < i) out.push(s.slice(run, i));
        out.push('<code>');
        escPush(s.slice(i + 1, j), out);
        out.push('</code>');
        i = j + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code === 42) {
      // *
      if (s.charCodeAt(i + 1) === 42) {
        const j = findUnescaped(s, '**', i + 2);
        if (j > i + 2) {
          if (run < i) out.push(s.slice(run, i));
          out.push('<strong>');
          inlineTo(s.slice(i + 2, j), env, out);
          out.push('</strong>');
          i = j + 2;
          run = i;
        } else {
          i += 2;
        }
        continue;
      }
      const j = findSingleStar(s, i + 1);
      if (j > i + 1) {
        if (run < i) out.push(s.slice(run, i));
        out.push('<em>');
        inlineTo(s.slice(i + 1, j), env, out);
        out.push('</em>');
        i = j + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code === 91) {
      // [
      const k = findUnescaped(s, ']', i + 1);
      const m = k !== -1 && s.charCodeAt(k + 1) === 40 ? s.indexOf(')', k + 2) : -1;
      const target = m === -1 ? '' : s.slice(k + 2, m);
      if (target !== '' && !WS_RE.test(target)) {
        if (run < i) out.push(s.slice(run, i));
        out.push('<a href="');
        escPush(env.link(target), out);
        out.push('">');
        inlineTo(s.slice(i + 1, k), env, out);
        out.push('</a>');
        i = m + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code === 38 || code === 60 || code === 62 || code === 34 || code === 39) {
      if (run < i) out.push(s.slice(run, i));
      out.push(ESCAPES[s[i]]);
      i++;
      run = i;
      continue;
    }
    i++;
  }
  if (run < n) out.push(s.slice(run));
}

/** `s` rendered inline, as a string. */
export function inline(s, env) {
  const out = [];
  inlineTo(s, env, out);
  return out.join('');
}

/**
 * `src` rendered: its HTML, its table of contents (level 2 and 3 headings)
 * and its excerpt (the first paragraph's inline HTML, or ""). Blocks render
 * straight into one output array; only headings (whose HTML the toc also
 * needs) and the first paragraph (the excerpt) are built as strings.
 */
export function renderMarkdown(src, env) {
  const lines = src.split('\n');
  const n = lines.length;
  const parts = [];
  const toc = [];
  const ids = new Set();
  let excerpt = null;
  let i = 0;
  while (i < n) {
    const line = lines[i];
    if (isBlank(line)) {
      i++;
      continue;
    }
    const c0 = line.charCodeAt(0);
    if (c0 === 96) {
      const lang = fenceOpen(line);
      if (lang !== null) {
        parts.push('<pre><code');
        if (lang !== '') {
          parts.push(' class="language-');
          escPush(lang, parts);
          parts.push('"');
        }
        parts.push('>');
        i++;
        const bodyLines = [];
        while (i < n && !isFenceClose(lines[i])) bodyLines.push(lines[i++]);
        i++;
        if (bodyLines.length > 0) escPush(`${bodyLines.join('\n')}\n`, parts);
        parts.push('</code></pre>\n');
        continue;
      }
    }
    if (c0 === 35) {
      const heading = headingOf(line);
      if (heading !== null) {
        const id = uniqueSlug(slugify(heading.text), ids);
        const html = inline(heading.text, env);
        parts.push(`<h${heading.depth} id="${id}">`, html, `</h${heading.depth}>\n`);
        if (heading.depth === 2 || heading.depth === 3) toc.push({ level: heading.depth, id, html });
        i++;
        continue;
      }
    }
    if (c0 === 45 || c0 === 42) {
      if (isHR(line)) {
        parts.push('<hr>\n');
        i++;
        continue;
      }
    }
    if (c0 === 62) {
      const stripped = [];
      while (i < n && lines[i].charCodeAt(0) === 62) {
        const l = lines[i++];
        stripped.push(l.slice(l.charCodeAt(1) === 32 ? 2 : 1));
      }
      const kept = [];
      for (const l of stripped) {
        const t = trimLine(l);
        if (t !== '') kept.push(t);
      }
      if (kept.length === 0) parts.push('<blockquote>\n</blockquote>\n');
      else {
        parts.push('<blockquote>\n<p>');
        inlineTo(kept.join('\n'), env, parts);
        parts.push('</p>\n</blockquote>\n');
      }
      continue;
    }
    const first = listStart(line);
    if (first !== null) {
      const isOrdered = first.ordered;
      const items = [[trimLine(first.text)]];
      i++;
      while (i < n) {
        const l = lines[i];
        const st = listStart(l);
        if (st !== null && st.ordered === isOrdered) {
          items.push([trimLine(st.text)]);
          i++;
        } else if (l.startsWith('  ') && CONTINUED.test(l)) {
          items[items.length - 1].push(trimLine(l));
          i++;
        } else break;
      }
      const tag = isOrdered ? 'ol' : 'ul';
      parts.push(isOrdered && first.start !== 1 ? `<ol start="${first.start}">\n` : `<${tag}>\n`);
      for (const item of items) {
        parts.push('<li>');
        inlineTo(item.join('\n'), env, parts);
        parts.push('</li>\n');
      }
      parts.push(`</${tag}>\n`);
      continue;
    }
    const body = [trimLine(line)];
    i++;
    while (i < n && !endsParagraph(lines[i])) body.push(trimLine(lines[i++]));
    if (excerpt === null) {
      const html = inline(body.join('\n'), env);
      excerpt = html;
      parts.push('<p>', html, '</p>\n');
    } else {
      parts.push('<p>');
      inlineTo(body.join('\n'), env, parts);
      parts.push('</p>\n');
    }
  }
  return { html: parts.join(''), toc, excerpt: excerpt ?? '' };
}

// ---------------------------------------------------------------- templates

export const INCLUDE_DEPTH = 8;

const PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF = /^if (not )?(\S+)$/;
const INCLUDE = /^include "([a-z0-9-]+)"$/;

/** A template that does not parse: its name and why. */
export class TemplateError extends Error {}

/** The path `text` names, split at its dots, or a TemplateError. */
function pathOf(name, text) {
  if (!PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

/** `src`'s tokens: text, {{ }}, {{{ }}} and {% %}, a standalone tag's line taken with it. */
function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

/** `src` parsed into its tree of nodes (a TemplateError when it does not parse). */
export function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = INCLUDE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

/** The value at `path` in the scope chain: its first name from the nearest scope holding it, then own properties. */
function lookup(scope, path) {
  let s = scope;
  while (s !== null && !Object.hasOwn(s.vars, path[0])) s = s.parent;
  if (s === null) return undefined;
  let v = s.vars[path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

/** A value as text: a string itself, a number in JavaScript's digits, true or false, an array's items joined by ", ", else "". */
function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

/** Whether a value counts as true in an if: not undefined, null, false, "", 0 or an empty array. */
function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

/** The root names every render carries, whatever the page: a template that
 * reads nothing else renders the same for every page of the case. */
const CASE_CONSTANTS = new Set(['site', 'tags', 'recent']);

/**
 * For each parsed template: whether its render is the same for every page
 * of the case (it reads only site/tags/recent, through its includes, and
 * nothing bound by a caller's loop), how deep its include chain runs, and
 * the one name its render depends on besides site/tags/recent, if exactly
 * one (a template reading only that name renders the same wherever that
 * name holds the same value, so it is rendered once per value and reused).
 */
function analyzeTemplates(templates) {
  const memo = new Map();
  const onStack = new Set();
  function analyze(name, bound) {
    const key = `${name}\0${[...bound].sort().join(',')}`;
    const hit = memo.get(key);
    if (hit !== undefined) return hit;
    if (onStack.has(key)) return { constant: false, maxRel: 99, keyVar: undefined, protRoots: null, cacheKind: null, roots: new Set(['\0cycle']) };
    const nodes = templates.get(name);
    if (nodes === undefined) return { constant: true, maxRel: -1, keyVar: null, protRoots: new Set(), cacheKind: 'const', roots: new Set() };
    onStack.add(key);
    let constant = true;
    let maxRel = -1;
    const roots = new Set();
    // `bound`: names bound by an ancestor's loop (they resolve to the
    // caller's element, so reading one is not constant). `own`: names bound
    // by this template's own loops (their path is checked where it stands).
    const check = (first, boundSet, own) => {
      if (!own.has(first)) roots.add(first);
      if (boundSet.has(first)) constant = false;
      else if (!own.has(first) && !CASE_CONSTANTS.has(first)) constant = false;
    };
    const walk = (ns, own) => {
      for (const n of ns) {
        switch (n.kind) {
          case 'var':
          case 'raw':
            check(n.path[0], bound, own);
            break;
          case 'if':
            check(n.path[0], bound, own);
            walk(n.then, own);
            if (n.else !== null) walk(n.else, own);
            break;
          case 'for': {
            check(n.path[0], bound, own);
            const own2 = new Set(own);
            own2.add(n.name);
            own2.add('loop');
            walk(n.body, own2);
            break;
          }
          case 'include': {
            const sub = analyze(n.name, new Set([...bound, ...own]));
            if (!sub.constant) constant = false;
            for (const r of sub.roots) if (!own.has(r)) roots.add(r);
            if (1 + sub.maxRel > maxRel) maxRel = 1 + sub.maxRel;
            break;
          }
        }
      }
    };
    walk(nodes, new Set());
    onStack.delete(key);
    const extra = [...roots].filter((r) => !CASE_CONSTANTS.has(r));
    const keyVar = extra.length === 0 ? null : extra.length === 1 && extra[0] !== 'loop' ? extra[0] : undefined;
    const protRoots = keyVar === undefined ? null : keyVar === null ? roots : new Set([...roots].filter((r) => r !== keyVar));
    const res = { constant, maxRel, keyVar, protRoots, cacheKind: keyVar === null ? 'const' : keyVar === undefined ? null : 'key', roots };
    memo.set(key, res);
    return res;
  }
  const info = new Map();
  for (const name of templates.keys()) info.set(name, analyze(name, new Set()));
  return info;
}

/**
 * `nodes` rendered into `out` in `scope` ({ vars, parent }). `ctx` holds
 * the parsed templates. Output is accumulated in an array and joined once
 * by the caller, never concatenated string by string.
 */
export function renderNodes(nodes, scope, ctx, depth, out) {
  for (const n of nodes) {
    switch (n.kind) {
      case 'text':
        out.push(n.text);
        break;
      case 'var':
        escPush(textOf(lookup(scope, n.path)), out);
        break;
      case 'raw':
        out.push(textOf(lookup(scope, n.path)));
        break;
      case 'if': {
        const v = truthy(lookup(scope, n.path));
        const branch = v !== n.not ? n.then : n.else;
        if (branch !== null) renderNodes(branch, scope, ctx, depth, out);
        break;
      }
      case 'for': {
        const xs = lookup(scope, n.path);
        if (!Array.isArray(xs)) break;
        for (let k = 0; k < xs.length; k++) {
          const vars = { [n.name]: xs[k], loop: { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length } };
          renderNodes(n.body, { vars, parent: scope, bound: n.name }, ctx, depth, out);
        }
        break;
      }
      case 'include': {
        const t = ctx.templates.get(n.name);
        if (t === undefined || depth >= INCLUDE_DEPTH) break;
        const info = ctx.constInfo.get(n.name);
        const childDepth = depth + 1;
        let done = false;
        if (info !== undefined && info.cacheKind !== null && childDepth + info.maxRel < INCLUDE_DEPTH) {
          // A cached render stays valid only when no enclosing loop binds
          // a name the template reads besides its key: a loop's element
          // would shadow the case-wide value the cache was built from.
          let shadowed = false;
          let s = scope;
          while (s !== null) {
            if (s.bound !== undefined && info.protRoots.has(s.bound)) {
              shadowed = true;
              break;
            }
            s = s.parent;
          }
          if (!shadowed) {
            if (info.cacheKind === 'const') {
              const hit = ctx.constCache.get(n.name);
              if (hit !== undefined && hit.depth + info.maxRel < INCLUDE_DEPTH) {
                out.push(hit.str);
                done = true;
              } else {
                const tmp = [];
                renderNodes(t, scope, ctx, childDepth, tmp);
                const str = tmp.join('');
                ctx.constCache.set(n.name, { depth: childDepth, str });
                out.push(str);
                done = true;
              }
            } else {
              const keyVal = lookup(scope, [info.keyVar]);
              let byVal = ctx.varCache.get(n.name);
              if (byVal === undefined) {
                byVal = new Map();
                ctx.varCache.set(n.name, byVal);
              }
              const hit = byVal.get(keyVal);
              if (hit !== undefined && hit.depth + info.maxRel < INCLUDE_DEPTH) {
                out.push(hit.str);
                done = true;
              } else {
                const tmp = [];
                renderNodes(t, scope, ctx, childDepth, tmp);
                const str = tmp.join('');
                byVal.set(keyVal, { depth: childDepth, str });
                out.push(str);
                done = true;
              }
            }
          }
        }
        if (!done) renderNodes(t, scope, ctx, childDepth, out);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- the build

export const REPORT_FILE = 'build-report.txt';
export const RESERVED = Object.freeze(['posts', 'tags', 'page']);

/** The words of a body: its runs of characters other than whitespace. */
function wordsOf(text) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    const ws = c === 32 || c === 9 || c === 10 || c === 11 || c === 12 || c === 13 || c === 160 || c === 0x1680 ||
      (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
    if (ws) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}

/** `rel` (a link's path, "/"-separated) from the directory `dir` of a source, or null when it climbs out of the root. */
function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');

/** A page's link record as templates read it. */
const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : longDate(s.data.date) });

/** One site's output tree: path -> text. */
export function build(input) {
  slugCache = new Map();
  const templates = new Map();
  const report = [];
  for (const name of Object.keys(input.templates).sort(byCodeUnits)) {
    try {
      templates.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }
  const b = {
    input,
    base: input.site.base_url.replace(/\/+$/, ''),
    templates,
    sources: [],
    statics: [],
    bySource: new Map(),
    broken: new Set(),
    report,
    posts: [],
    tags: [],
    cards: new Map(),
    output: {},
  };

  // 1. The sources, in code-unit order of their paths.
  const files = [...b.input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  for (const f of files) {
    if (f.path.endsWith('.md')) b.sources.push({ path: f.path, text: f.content.replace(/\r\n?/g, '\n') });
    else b.statics.push({ path: f.path, content: f.content });
  }

  // 2. Each source's front matter: refused or draft sources are reported and dropped.
  const kept = [];
  for (const s of b.sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      b.report.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    const own = RESERVED.find((d) => d !== 'posts' && s.path.startsWith(`${d}/`));
    if (own !== undefined) {
      b.report.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !Object.hasOwn(s.data, 'date')) {
      b.report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!b.templates.has(layout)) {
      b.report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      b.report.push(`draft ${s.path}`);
      continue;
    }
    kept.push(s);
  }
  b.sources = kept;

  // 3. Each page's output directory and url.
  const taken = new Map();
  const takenIn = (dir) => {
    if (!taken.has(dir)) taken.set(dir, new Set(dir === '' ? RESERVED : []));
    return taken.get(dir);
  };
  for (const s of b.sources) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), takenIn(dir));
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${b.base}/${s.outDir}/`;
    b.bySource.set(s.path, s);
  }

  // 4. Each body's Markdown, its links resolved.
  const staticPaths = new Set(b.statics.map((f) => f.path));
  for (const s of b.sources) {
    const dir = dirOf(s.path);
    const env = {
      link: (target) => {
        if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(target) || target.startsWith('/') || target.startsWith('#')) return target;
        const hash = target.indexOf('#');
        const rel = hash === -1 ? target : target.slice(0, hash);
        const frag = hash === -1 ? '' : target.slice(hash);
        const path = rel === '' ? null : resolvePath(dir, rel);
        if (path !== null && b.bySource.has(path)) return `${b.bySource.get(path).url}${frag}`;
        if (path !== null && staticPaths.has(path)) return `${b.base}/${path}${frag}`;
        b.broken.add(`broken-link ${s.path} -> ${target}`);
        return target;
      },
    };
    const md = renderMarkdown(s.body, env);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // 5. The posts, newest first and then by slug; each post's newer and older neighbours.
  const posts = b.sources.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  b.posts = posts;
  posts.forEach((p, k) => {
    p.newer = k === 0 ? null : posts[k - 1];
    p.older = k === posts.length - 1 ? null : posts[k + 1];
  });

  // 6. The tags, grouped by slug, ordered by count and then slug.
  const bySlug = new Map();
  const ownTags = (s) => {
    const seen = new Set();
    const out = [];
    for (const name of s.data.tags ?? []) {
      const slug = slugify(name);
      if (!seen.has(slug)) {
        seen.add(slug);
        out.push([name, slug]);
      }
    }
    return out;
  };
  for (const p of b.posts) {
    p.tagRefs = [];
    for (const [name, slug] of ownTags(p)) {
      if (!bySlug.has(slug)) bySlug.set(slug, { name, slug, url: `${b.base}/tags/${slug}/`, posts: [] });
      const tag = bySlug.get(slug);
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of b.sources) {
    if (!s.post) {
      s.tagRefs = ownTags(s).map(([name, slug]) => bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
    }
  }
  b.tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  // 7. Every page through its layout, the index pages and the tag pages.
  const cardOf = (s) => {
    if (s._card !== undefined) return s._card;
    const c = {
      title: s.data.title,
      url: s.url,
      date: s.data.date ?? '',
      date_long: s.data.date === undefined ? '' : longDate(s.data.date),
      summary: s.data.summary ?? '',
      excerpt: s.excerpt,
      reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
      tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
    };
    s._card = c;
    return c;
  };
  const pageOf = (s) => ({
    ...cardOf(s),
    slug: s.slug,
    content: s.html,
    toc: s.toc.map((h) => ({ level: h.level, id: h.id, html: h.html })),
    newer: linkOf(s.newer ?? null),
    older: linkOf(s.older ?? null),
    is_post: s.post,
  });
  const tagOf = (t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cardOf(p)) });
  b.cards = new Map(b.posts.map((p) => [p, cardOf(p)]));
  const site = { title: b.input.site.title, base_url: b.base };
  const allTags = b.tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = b.posts.slice(0, b.input.site.recent).map((p) => b.cards.get(p));
  const ctx = { templates: b.templates, constInfo: analyzeTemplates(b.templates), constCache: new Map(), varCache: new Map() };
  const render = (name, vars) => {
    const nodes = b.templates.get(name);
    if (nodes === undefined) return null;
    const out = [];
    renderNodes(nodes, { vars: { site, tags: allTags, recent, ...vars }, parent: null }, ctx, 0, out);
    return out.join('');
  };
  for (const s of b.sources) b.output[`${s.outDir}/index.html`] = render(s.layout, { page: pageOf(s) });
  const per = b.input.site.per_page;
  const pages = Math.max(1, Math.ceil(b.posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${b.base}/` : `${b.base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const html = render('index', {
      posts: b.posts.slice((k - 1) * per, k * per).map((p) => b.cards.get(p)),
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) b.output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (const t of b.tags) {
    const html = render('tag', { tag: tagOf(t) });
    if (html !== null) b.output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', {});
  if (tagsHtml !== null) b.output['tags/index.html'] = tagsHtml;
  for (const f of b.statics) {
    if (Object.hasOwn(b.output, f.path)) b.report.push(`shadowed ${f.path}: a built page has its path`);
    else b.output[f.path] = f.content;
  }

  // 8. The report.
  const lines = [...b.report, ...b.broken].sort(byCodeUnits);
  const pageCount = b.sources.filter((s) => !s.post).length;
  lines.push(`built ${pageCount} pages, ${b.posts.length} posts, ${b.tags.length} tags and ${b.statics.length} static files`);
  b.output[REPORT_FILE] = `${lines.join('\n')}\n`;
  return b.output;
}

/** makefun-site 1.0.0's answer for one case's input. */
export function run(input) {
  return build(input);
}
