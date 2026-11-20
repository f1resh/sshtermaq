import { describe, expect, it } from 'vitest';
import { finishedText, finishedTitle, fmtElapsed, queueDoneText, ranFor } from './transferTimes';

const T0 = Date.UTC(2026, 10, 19, 13, 2, 11);
type Over = Partial<Parameters<typeof finishedText>[0]>;
const t = (over: Over = {}) => ({
  state: 'done' as const,
  direction: 'download' as const,
  total: 845_112,
  transferred: 845_112,
  startedAt: T0,
  finishedAt: T0 + 400,
  ...over
});

describe('fmtElapsed', () => {
  it('reads like a person would say it', () => {
    expect(fmtElapsed(400)).toBe('0.4 s');
    expect(fmtElapsed(2_340)).toBe('2.3 s');
    expect(fmtElapsed(41_000)).toBe('41 s');
    expect(fmtElapsed(192_000)).toBe('3 min 12 s');
    expect(fmtElapsed(180_000)).toBe('3 min');
    expect(fmtElapsed(3_720_000)).toBe('1 h 2 min');
    expect(fmtElapsed(7_200_000)).toBe('2 h');
    expect(fmtElapsed(-1)).toBe('');
  });
});

describe('a finished transfer', () => {
  it('a file: its size and how long it took', () => {
    expect(finishedText(t())).toBe('Done · 825 KB in 0.4 s');
  });

  it('a folder: the file count, with skipped files', () => {
    const folder = t({ filesTotal: 3000, filesDone: 3000, total: 24_535_219, transferred: 24_535_219, finishedAt: T0 + 41_000 });
    expect(finishedText(folder)).toBe('Done · 3,000 files · 23 MB in 41 s');
    expect(finishedText({ ...folder, skipped: 12 })).toBe('Done · 3,000 files, 12 skipped · 23 MB in 41 s');
  });

  it('failed and cancelled ones say how long they ran', () => {
    expect(finishedText(t({ state: 'error', finishedAt: T0 + 12_000 }))).toBe('Failed after 12 s');
    expect(finishedText(t({ state: 'cancelled', finishedAt: T0 + 3_000 }))).toBe('Cancelled after 3.0 s');
  });

  it('without times (never ran, or an older host) it reads as before', () => {
    const none = { startedAt: undefined, finishedAt: undefined };
    expect(ranFor(t(none))).toBeNull();
    expect(finishedText(t(none))).toBe('825 KB');
    expect(finishedText(t({ ...none, skipped: 2 }))).toBe('Done, 2 skipped');
    expect(finishedText(t({ ...none, state: 'cancelled' }))).toBe('Cancelled');
    expect(finishedText(t({ ...none, state: 'error' }))).toBe('Failed');
    expect(finishedText(t({ ...none, direction: 'delete' }))).toBe('Deleted');
  });

  it('the tooltip has the average speed and the clock times', () => {
    const title = finishedTitle(t({ finishedAt: T0 + 2_000 }));
    expect(title).toMatch(/^Average 413 KB\/s · \d\d:\d\d:\d\d – \d\d:\d\d:\d\d$/);
    expect(finishedTitle(t({ startedAt: undefined }))).toBe('');
  });
});

describe('the queue summary once everything has ended', () => {
  it('total bytes from the first start to the last end', () => {
    const list = [
      t({ total: 1024 * 1024, transferred: 1024 * 1024, startedAt: T0, finishedAt: T0 + 5_000 }),
      t({ total: 1024 * 1024, transferred: 1024 * 1024, startedAt: T0 + 1_000, finishedAt: T0 + 9_000 }),
      t({ state: 'cancelled', transferred: 500, startedAt: T0 + 2_000, finishedAt: T0 + 3_000 })
    ];
    expect(queueDoneText(list)).toBe('All done · 2.0 MB in 9.0 s');
  });

  it('"All done" alone without times', () => {
    expect(queueDoneText([t({ startedAt: undefined, finishedAt: undefined })])).toBe('All done');
  });
});
