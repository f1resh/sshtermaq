/**
 * The version check in the running app (1.2.0 item 12): a few seconds after start, and then whenever a day has passed
 * (checked every hour, for an app left open for weeks), one `update:check` if "Check for new versions" is on. Never in
 * a Store package (the Store updates it) or with --no-update-check (AppInfo.updateChecks). "Check now" asks at once.
 *
 * What it found shows as a chip in the title bar (UpdateChip) and a dot on Settings and Settings › About; the times,
 * the skipped version and "remind me tomorrow" are kept in state.json (updatesModel.STATE_KEY). It never downloads
 * or installs anything: the release page opens in the browser.
 */
import { create } from 'zustand';
import { api } from '@/bridge/api';
import { toast } from '@/components/ui';
import { useSettings } from '@/store/data';
import type { InstallChannel } from '@shared/types';
import {
  DAY_MS,
  FIRST_CHECK_DELAY_MS,
  STATE_KEY,
  TICK_MS,
  checkDue,
  isReleasePage,
  offered,
  type Release,
  type UpdateState
} from './updatesModel';

interface UpdatesStore {
  /** Known once AppInfo has arrived. */
  current: string;
  channel: InstallChannel;
  /** Checks are possible in this copy (not the Store, no --no-update-check). */
  available: boolean;
  state: UpdateState;
  /** The release the title bar offers, or null. */
  offer: Release | null;
  checking: boolean;
  /** The last check of this run: its outcome for Settings › About. */
  outcome?: { ok: true; newer: boolean } | { ok: false; error: string };
}

export const useUpdates = create<UpdatesStore>(() => ({
  current: '',
  channel: 'other',
  available: false,
  state: {},
  offer: null,
  checking: false
}));

const enabled = () => useSettings.getState().settings.behavior.checkForUpdates !== false;

function apply(state: UpdateState, persist = true): void {
  const s = useUpdates.getState();
  useUpdates.setState({ state, offer: offered(state, s.current, Date.now()) });
  if (persist) void api.app.setState(STATE_KEY, state).catch(() => undefined);
}

/** One check. `manual` ("Check now"): also when the setting is off, and it brings back a version put off until tomorrow. */
export async function checkForUpdates(manual = false): Promise<void> {
  const s = useUpdates.getState();
  if (!s.available || s.checking) return;
  if (!manual && !enabled()) return;
  useUpdates.setState({ checking: true });
  const state: UpdateState = { ...useUpdates.getState().state, lastCheck: Date.now() };
  if (manual) delete state.remindAfter;
  try {
    const r = await api.update.check();
    if (r.ok && r.latest) {
      state.latest = r.latest;
      useUpdates.setState({ outcome: { ok: true, newer: r.newer } });
    } else {
      useUpdates.setState({ outcome: { ok: false, error: r.error || 'No answer' } });
    }
  } catch (e) {
    useUpdates.setState({ outcome: { ok: false, error: e instanceof Error ? e.message : String(e) } });
  } finally {
    useUpdates.setState({ checking: false });
    apply(state);
  }
}

/** Loads what the last run knew and starts the timers; returns the stop function. */
export function startUpdateChecks(): () => void {
  let stopped = false;
  const timers: ReturnType<typeof setTimeout>[] = [];
  const tick = () => {
    if (!stopped && checkDue(useUpdates.getState().state, Date.now())) void checkForUpdates();
  };
  void (async () => {
    try {
      const info = await api.app.info();
      const saved = ((await api.app.getState<UpdateState>(STATE_KEY).catch(() => undefined)) ?? {}) as UpdateState;
      if (stopped) return;
      useUpdates.setState({ current: info.version, channel: info.installChannel ?? 'other', available: info.updateChecks !== false });
      if (info.updateChecks === false) return;
      apply(saved, false);
      timers.push(setTimeout(tick, FIRST_CHECK_DELAY_MS));
      timers.push(setInterval(tick, TICK_MS));
    } catch {
      /* no version check without AppInfo */
    }
  })();
  return () => {
    stopped = true;
    timers.forEach((t) => clearTimeout(t));
  };
}

export function remindTomorrow(): void {
  apply({ ...useUpdates.getState().state, remindAfter: Date.now() + DAY_MS });
}

export function skipVersion(version: string): void {
  apply({ ...useUpdates.getState().state, skipped: version });
}

export function openReleasePage(release: Release): void {
  if (!isReleasePage(release.url)) return;
  void api.app.openExternal(release.url).catch((e) => toast.error('Could not open the release page', e instanceof Error ? e.message : String(e)));
}

/** The title bar's chip and the dots show only while the setting is on. */
export function useOffer(): Release | null {
  const offer = useUpdates((s) => s.offer);
  const on = useSettings((s) => s.settings.behavior.checkForUpdates !== false);
  return on ? offer : null;
}
