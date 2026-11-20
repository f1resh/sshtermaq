/**
 * What a finished transfer shows: its size, file count and how long it took ("Done · 3,000 files · 23.4 MB in 41 s"),
 * from the start and end times the transfer engine records (TransferProgress.startedAt / finishedAt). Pure: unit
 * tested in transferTimes.test.ts; used by the file manager's queue and the SFTP panel.
 */
import type { TransferProgress } from '@shared/types';
import { fmtSize } from './paths';

type Timed = Pick<TransferProgress, 'state' | 'direction' | 'total' | 'transferred' | 'filesTotal' | 'filesDone' | 'skipped' | 'startedAt' | 'finishedAt'>;

/** "0.4 s", "2.3 s", "41 s", "3 min 12 s", "1 h 2 min". */
export function fmtElapsed(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '';
  const s = ms / 1000;
  if (s < 10) return `${(Math.round(s * 10) / 10).toFixed(1)} s`;
  if (s < 60) return `${Math.round(s)} s`;
  const total = Math.round(s);
  if (total < 3600) {
    const min = Math.floor(total / 60);
    const sec = total % 60;
    return sec ? `${min} min ${sec} s` : `${min} min`;
  }
  const h = Math.floor(total / 3600);
  const min = Math.floor((total % 3600) / 60);
  return min ? `${h} h ${min} min` : `${h} h`;
}

/** How long the transfer ran, in ms; null when it never ran or has not ended (or an older host sent no times). */
export function ranFor(t: Timed): number | null {
  if (!t.startedAt || !t.finishedAt || t.finishedAt < t.startedAt) return null;
  return t.finishedAt - t.startedAt;
}

const count = (n: number) => n.toLocaleString('en-US');

/** The bytes a finished transfer moved (the total once done; skipped files are already taken out of it). */
const movedBytes = (t: Timed) => (t.state === 'done' ? t.total : t.transferred);

/**
 * The status text of a transfer that has ended. Without times, as before: the size, "Failed", "Cancelled".
 * Done: "Done · 845.1 KB in 0.4 s"; a folder: "Done · 3,000 files · 23.4 MB in 41 s"; with skipped files:
 * "Done · 3,000 files, 12 skipped · 23.4 MB in 41 s". Failed / cancelled: "Failed after 12 s".
 */
export function finishedText(t: Timed): string {
  const ms = ranFor(t);
  const took = ms === null ? '' : ` in ${fmtElapsed(ms)}`;
  switch (t.state) {
    case 'error':
      return ms === null ? 'Failed' : `Failed after ${fmtElapsed(ms)}`;
    case 'cancelled':
      return ms === null ? 'Cancelled' : `Cancelled after ${fmtElapsed(ms)}`;
    case 'done': {
      if (t.direction === 'delete') return ms === null ? 'Deleted' : `Deleted${took}`;
      if (ms === null) return t.skipped ? `Done, ${t.skipped} skipped` : fmtSize(t.total);
      const files = t.filesTotal && t.filesTotal > 1 ? `${count(t.filesTotal)} files` : '';
      const skipped = t.skipped ? `${t.skipped} skipped` : '';
      const what = [files, skipped].filter(Boolean).join(', ');
      return ['Done', what, `${fmtSize(movedBytes(t))}${took}`].filter(Boolean).join(' · ');
    }
    default:
      return '';
  }
}

const clock = (ms: number) => new Date(ms).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', second: '2-digit' });

/** Tooltip of an ended transfer: the average speed and the start and end times ("Average 2.0 MB/s · 14:02:11 – 14:02:52"). */
export function finishedTitle(t: Timed): string {
  const ms = ranFor(t);
  if (ms === null) return '';
  const bytes = movedBytes(t);
  const speed = ms > 0 && bytes > 0 ? `Average ${fmtSize(Math.round((bytes / ms) * 1000))}/s · ` : '';
  return `${speed}${clock(t.startedAt!)} – ${clock(t.finishedAt!)}`;
}

/**
 * The queue's summary once nothing is active: "All done · 23.4 MB in 41 s", from the first start to the last end of
 * the transfers that ran and the bytes the finished ones moved. "All done" alone when no times are known.
 */
export function queueDoneText(list: Timed[]): string {
  let first = Infinity;
  let last = 0;
  let bytes = 0;
  for (const t of list) {
    if (ranFor(t) === null) continue;
    first = Math.min(first, t.startedAt!);
    last = Math.max(last, t.finishedAt!);
    if (t.state === 'done' && t.direction !== 'delete') bytes += movedBytes(t);
  }
  if (!last) return 'All done';
  return `All done · ${fmtSize(bytes)} in ${fmtElapsed(last - first)}`;
}
