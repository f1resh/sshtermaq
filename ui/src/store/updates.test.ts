import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createDefaultSettings } from '@shared/defaults';
import type { AppInfo, UpdateCheckResult } from '@shared/types';
import { api } from '@/bridge/api';
import { useSettings } from '@/store/data';
import {
  DAY_MS,
  FIRST_CHECK_DELAY_MS,
  STATE_KEY,
  TICK_MS,
  checkDue,
  isNewer,
  isReleasePage,
  offered,
  parseVersion,
  upgradeCommand,
  type UpdateState
} from './updatesModel';

vi.mock('@/components/ui', () => ({ toast: { info: vi.fn(), error: vi.fn() } }));
const { checkForUpdates, openReleasePage, remindTomorrow, skipVersion, startUpdateChecks, useUpdates } = await import('./updates');

const page = (v: string) => `https://github.com/f1resh/sshtermaq/releases/tag/${v}`;
const rel = (v: string) => ({ version: v, url: page(v) });

describe('update rules', () => {
  it('versions are N.N.N (an optional v) and compare as numbers', () => {
    expect(parseVersion('1.2.0')).toEqual([1, 2, 0]);
    expect(parseVersion('v1.10.3')).toEqual([1, 10, 3]);
    for (const bad of ['', '1.2', '1.2.0.4', '1.2.0-beta', ' 1.2.0', 'x.y.z', '123456.0.0']) expect(parseVersion(bad)).toBeNull();
    expect(isNewer('1.10.0', '1.2.0')).toBe(true);  // not as text
    expect(isNewer('1.2.1', '1.2.0')).toBe(true);
    expect(isNewer('2.0.0', '1.99.99')).toBe(true);
    expect(isNewer('1.2.0', '1.2.0')).toBe(false);
    expect(isNewer('1.1.9', '1.2.0')).toBe(false);
    expect(isNewer('junk', '1.2.0')).toBe(false);
    expect(isNewer('1.3.0', 'dev')).toBe(false);
  });

  it('a check is due once a day (also after the clock went back)', () => {
    const now = 1_800_000_000_000;
    expect(checkDue({}, now)).toBe(true);
    expect(checkDue({ lastCheck: now - DAY_MS + 1 }, now)).toBe(false);
    expect(checkDue({ lastCheck: now - DAY_MS }, now)).toBe(true);
    expect(checkDue({ lastCheck: now + 60_000 }, now)).toBe(true);
  });

  it('the notice: newer, not skipped, not put off until tomorrow', () => {
    const now = 1_800_000_000_000;
    const s: UpdateState = { latest: rel('1.3.0') };
    expect(offered(s, '1.2.0', now)).toEqual(rel('1.3.0'));
    expect(offered(s, '1.3.0', now)).toBeNull();  // updated meanwhile
    expect(offered({ ...s, skipped: '1.3.0' }, '1.2.0', now)).toBeNull();
    expect(offered({ latest: rel('1.4.0'), skipped: '1.3.0' }, '1.2.0', now)).toEqual(rel('1.4.0'));  // the next one is
    expect(offered({ ...s, remindAfter: now + 1 }, '1.2.0', now)).toBeNull();
    expect(offered({ ...s, remindAfter: now }, '1.2.0', now)).toEqual(rel('1.3.0'));
    expect(offered({}, '1.2.0', now)).toBeNull();
  });

  it('only the project release pages open; Homebrew has its own command', () => {
    expect(isReleasePage(page('1.3.0'))).toBe(true);
    expect(isReleasePage(page('v1.3.0'))).toBe(true);
    for (const bad of ['https://github.com/f1resh/sshtermaq/releases/tag/1.3.0/../x', 'https://evil.example/f1resh/sshtermaq/releases/tag/1.3.0', 'http://github.com/f1resh/sshtermaq/releases/tag/1.3.0', page('1.3.0') + '?x=1'])
      expect(isReleasePage(bad)).toBe(false);
    expect(upgradeCommand('homebrew')).toBe('brew upgrade --cask sshtermaq');
    expect(upgradeCommand('other')).toBeNull();
    expect(upgradeCommand('store')).toBeNull();
  });
});

describe('the version check in the app', () => {
  let check: ReturnType<typeof vi.spyOn>;
  let setState: ReturnType<typeof vi.spyOn>;
  let saved: UpdateState | undefined;
  let stop: (() => void) | undefined;
  const info = (patch: Partial<AppInfo> = {}) =>
    vi.spyOn(api.app, 'info').mockResolvedValue({ version: '1.2.0', installChannel: 'other', updateChecks: true, ...patch } as AppInfo);
  const answer = (r: Partial<UpdateCheckResult>) => check.mockResolvedValue({ ok: true, current: '1.2.0', newer: false, ...r } as UpdateCheckResult);
  const withSetting = (on: boolean) => {
    const settings = createDefaultSettings();
    settings.behavior.checkForUpdates = on;
    useSettings.setState({ settings });
  };
  /** Starts the checks and lets the bridge calls answer. */
  async function start() {
    stop = startUpdateChecks();
    await vi.advanceTimersByTimeAsync(0);
  }

  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(1_800_000_000_000);
    useUpdates.setState({ current: '', channel: 'other', available: false, state: {}, offer: null, checking: false, outcome: undefined });
    withSetting(true);
    saved = undefined;
    check = vi.spyOn(api.update, 'check');
    answer({ latest: rel('1.2.0') });
    vi.spyOn(api.app, 'getState').mockImplementation(async () => saved as never);
    setState = vi.spyOn(api.app, 'setState').mockImplementation(async (_k, v) => void (saved = v as UpdateState));
    info();
  });
  afterEach(() => {
    stop?.();
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  it('asks a few seconds after start, then not again the same day; a newer release is offered', async () => {
    answer({ latest: rel('1.3.0'), newer: true });
    await start();
    expect(check).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS);
    expect(check).toHaveBeenCalledTimes(1);
    expect(useUpdates.getState().offer).toEqual(rel('1.3.0'));
    expect(setState).toHaveBeenLastCalledWith(STATE_KEY, { lastCheck: Date.now(), latest: rel('1.3.0') });
    await vi.advanceTimersByTimeAsync(TICK_MS * 5);
    expect(check).toHaveBeenCalledTimes(1);
  });

  it('a window left open asks again once a day has passed', async () => {
    await start();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS);
    await vi.advanceTimersByTimeAsync(DAY_MS + TICK_MS);
    expect(check).toHaveBeenCalledTimes(2);
  });

  it('a check earlier today (an earlier run): no request at start, and what it found is offered at once', async () => {
    saved = { lastCheck: Date.now() - 60_000, latest: rel('1.3.0') };
    await start();
    expect(useUpdates.getState().offer).toEqual(rel('1.3.0'));
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS + TICK_MS);
    expect(check).not.toHaveBeenCalled();
  });

  it('the setting off: no request; "Check now" still asks', async () => {
    withSetting(false);
    await start();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS + DAY_MS * 2);
    expect(check).not.toHaveBeenCalled();
    await checkForUpdates(true);
    expect(check).toHaveBeenCalledTimes(1);
  });

  it('a Store package or --no-update-check: no request at all, not even "Check now"', async () => {
    info({ installChannel: 'store', updateChecks: false });
    await start();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS + DAY_MS * 2);
    await checkForUpdates(true);
    expect(check).not.toHaveBeenCalled();
    expect(useUpdates.getState().available).toBe(false);
  });

  it('"Remind me tomorrow" hides it for a day; "Check now" brings it back', async () => {
    answer({ latest: rel('1.3.0'), newer: true });
    await start();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS);
    remindTomorrow();
    expect(useUpdates.getState().offer).toBeNull();
    expect(saved?.remindAfter).toBe(Date.now() + DAY_MS);
    await checkForUpdates(true);
    expect(useUpdates.getState().offer).toEqual(rel('1.3.0'));
    expect(saved?.remindAfter).toBeUndefined();
  });

  it('"Skip this version": not offered again, also after "Check now"; the next version is', async () => {
    answer({ latest: rel('1.3.0'), newer: true });
    await start();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS);
    skipVersion('1.3.0');
    expect(useUpdates.getState().offer).toBeNull();
    await checkForUpdates(true);
    expect(useUpdates.getState().offer).toBeNull();
    answer({ latest: rel('1.3.1'), newer: true });
    await checkForUpdates(true);
    expect(useUpdates.getState().offer).toEqual(rel('1.3.1'));
  });

  it('a failed check keeps what was known, says why, and counts as the day\'s check', async () => {
    saved = { lastCheck: Date.now() - DAY_MS * 2, latest: rel('1.3.0') };
    check.mockResolvedValue({ ok: false, error: 'the server answered 403', current: '1.2.0', newer: false });
    await start();
    await vi.advanceTimersByTimeAsync(FIRST_CHECK_DELAY_MS);
    expect(useUpdates.getState().outcome).toEqual({ ok: false, error: 'the server answered 403' });
    expect(useUpdates.getState().offer).toEqual(rel('1.3.0'));
    expect(saved?.lastCheck).toBe(Date.now());
    check.mockRejectedValue(new Error('offline'));
    await checkForUpdates(true);
    expect(useUpdates.getState().outcome).toEqual({ ok: false, error: 'offline' });
  });

  it('opens only a release page of the project', () => {
    const open = vi.spyOn(api.app, 'openExternal').mockResolvedValue();
    openReleasePage(rel('1.3.0'));
    openReleasePage({ version: '1.3.0', url: 'https://evil.example/' });
    expect(open).toHaveBeenCalledTimes(1);
    expect(open).toHaveBeenCalledWith(page('1.3.0'));
  });
});
