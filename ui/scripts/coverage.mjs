/**
 * UI line coverage of the unit tests (Vitest), the browser tests (Playwright) and both together, by area.
 *
 *   npm run coverage                 runs both suites with coverage, then prints the table (about 6 minutes)
 *   npm run coverage -- --report     only prints the table again from the last run's data (coverage/)
 *
 * Counted: the files of src/ (not the tests, not the mock backend: see `test.coverage` in vite.config.ts). A line
 * counts when a statement starts on it (as Vitest's report sees it); it is covered when a statement there ran in a unit
 * test, or when code mapped to that line ran in the browser during a Playwright test (PW_COV makes e2e/fixtures.ts
 * record V8 coverage; Vite's inline source maps lead back to the source lines).
 *
 * Writes coverage/summary.md (the table) and coverage/summary.json (per file).
 */
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { TraceMap, eachMapping } from '@jridgewell/trace-mapping';

const ui = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const out = join(ui, 'coverage');
const unitFile = join(out, 'unit', 'coverage-final.json');
const e2eDir = join(out, 'e2e');
const reportOnly = process.argv.includes('--report');

/* ---- 1. run the suites ------------------------------------------------------------------------------------------ */
function run(what, args, env = {}) {
  console.log(`\n== ${what}`);
  const r = spawnSync(process.execPath, args, { cwd: ui, stdio: 'inherit', env: { ...process.env, ...env } });
  if (r.status !== 0) console.warn(`(${what} reported failures: the coverage below still counts what ran)`);
}
if (!reportOnly) {
  rmSync(out, { recursive: true, force: true });
  mkdirSync(out, { recursive: true });
  run('Unit tests (Vitest) with coverage', [join(ui, 'node_modules/vitest/vitest.mjs'), 'run', '--coverage']);
  run('Browser tests (Playwright) with coverage', [join(ui, 'node_modules/@playwright/test/cli.js'), 'test', '--reporter=dot'], { PW_COV: e2eDir });
}
if (!existsSync(unitFile)) throw new Error(`No unit coverage at ${unitFile}: run without --report first`);

/* ---- 2. unit tests: Vitest's (Istanbul) report --------------------------------------------------------------------- */
/** Per file ("src/..."): the lines that count, and those covered by the unit tests. */
const files = new Map();
const rel = (abs) => relative(ui, abs).replace(/\\/g, '/');
for (const [abs, fc] of Object.entries(JSON.parse(readFileSync(unitFile, 'utf8')))) {
  const f = { lines: new Set(), unit: new Set(), e2e: new Set() };
  for (const [id, loc] of Object.entries(fc.statementMap)) {
    f.lines.add(loc.start.line);
    if (fc.s[id] > 0) f.unit.add(loc.start.line);
  }
  files.set(rel(abs), f);
}

/* ---- 3. browser tests: V8 ranges mapped through Vite's inline source maps ------------------------------------------ */
/** Per module source (by hash): its mappings as [generated offset, original line], sorted by offset. */
const mappingsCache = new Map();
function mappingsOf(hash) {
  if (mappingsCache.has(hash)) return mappingsCache.get(hash);
  const source = readFileSync(join(e2eDir, 'sources', `${hash}.js`), 'utf8');
  const m = /\/\/# sourceMappingURL=data:application\/json;(?:charset=utf-8;)?base64,([A-Za-z0-9+/=]+)\s*$/.exec(source);
  let list = [];
  if (m) {
    const lineStarts = [0];
    for (let i = 0; i < source.length; i++) if (source.charCodeAt(i) === 10) lineStarts.push(i + 1);
    const map = new TraceMap(JSON.parse(Buffer.from(m[1], 'base64').toString('utf8')));
    eachMapping(map, (s) => {
      if (s.originalLine != null) list.push([lineStarts[s.generatedLine - 1] + s.generatedColumn, s.originalLine]);
    });
    list.sort((a, b) => a[0] - b[0]);
  }
  mappingsCache.set(hash, list);
  return list;
}

/** Original lines whose code ran: for each mapped offset, the innermost V8 range around it decides (count > 0). */
function coveredLines(functions, mappings) {
  const ranges = functions.flatMap((fn) => fn.ranges).sort((a, b) => a.startOffset - b.startOffset || b.endOffset - a.endOffset);
  const lines = new Set();
  const stack = [];
  let next = 0;
  for (const [offset, line] of mappings) {
    while (next < ranges.length && ranges[next].startOffset <= offset) {
      const r = ranges[next++];
      while (stack.length && stack[stack.length - 1].endOffset <= r.startOffset) stack.pop();
      stack.push(r);
    }
    while (stack.length && stack[stack.length - 1].endOffset <= offset) stack.pop();
    if (stack.length && stack[stack.length - 1].count > 0) lines.add(line);
  }
  return lines;
}

let runs = 0;
if (existsSync(join(e2eDir, 'runs'))) {
  for (const name of readdirSync(join(e2eDir, 'runs'))) {
    runs++;
    for (const mod of JSON.parse(readFileSync(join(e2eDir, 'runs', name), 'utf8'))) {
      const f = files.get(mod.path.slice(1));
      if (!f) continue;
      for (const line of coveredLines(mod.functions, mappingsOf(mod.hash))) if (f.lines.has(line)) f.e2e.add(line);
    }
  }
}

/* ---- 4. the table ------------------------------------------------------------------------------------------------- */
/** "components/files", "components/dialogs/import", "store", ... */
function areaOf(path) {
  const parts = path.replace(/^src\//, '').split('/');
  if (parts.length === 1) return '(top level)';
  if (parts[0] === 'components' && parts[1] === 'dialogs' && parts.length > 3) return parts.slice(0, 3).join('/');
  if (parts[0] === 'components') return parts.slice(0, 2).join('/');
  return parts[0];
}
const sum = () => ({ lines: 0, unit: 0, e2e: 0, both: 0 });
const add = (acc, f) => {
  acc.lines += f.lines.size;
  acc.unit += f.unit.size;
  acc.e2e += f.e2e.size;
  acc.both += new Set([...f.unit, ...f.e2e]).size;
};
const total = sum();
const areas = new Map();
for (const [path, f] of files) {
  add(total, f);
  if (!areas.has(areaOf(path))) areas.set(areaOf(path), sum());
  add(areas.get(areaOf(path)), f);
}
const pct = (n, of) => (of ? `${Math.round((n / of) * 100)} %` : '—');
const rows = [...areas.entries()].sort((a, b) => a[1].both / a[1].lines - b[1].both / b[1].lines);
const table = [
  `UI line coverage, ${new Date().toISOString().slice(0, 10)}: ${files.size} files of src/, ${total.lines} lines; ${runs} browser test runs.`,
  '',
  '| Area | Lines | Unit | Browser | Both |',
  '|---|---:|---:|---:|---:|',
  `| **All** | ${total.lines} | ${pct(total.unit, total.lines)} | ${pct(total.e2e, total.lines)} | **${pct(total.both, total.lines)}** |`,
  ...rows.map(([area, a]) => `| \`${area}\` | ${a.lines} | ${pct(a.unit, a.lines)} | ${pct(a.e2e, a.lines)} | ${pct(a.both, a.lines)} |`)
].join('\n');
console.log(`\n${table}\n`);
writeFileSync(join(out, 'summary.md'), table + '\n');
writeFileSync(
  join(out, 'summary.json'),
  JSON.stringify(
    Object.fromEntries([...files].map(([p, f]) => [p, { lines: f.lines.size, unit: f.unit.size, e2e: f.e2e.size, both: new Set([...f.unit, ...f.e2e]).size }])),
    null,
    1
  )
);
console.log(`Written: ${relative(ui, join(out, 'summary.md'))}, ${relative(ui, join(out, 'summary.json'))}`);
