/**
 * The version check's rules (1.2.0 item 12), kept apart from timers and the bridge so they can be tested: when a
 * check is due, and whether the notice shows. The state is kept in state.json under STATE_KEY, never in settings.
 */
export const STATE_KEY = 'updateCheck';
export const DAY_MS = 24 * 60 * 60 * 1000;
/** The first check, after start (the window and the restored tabs first). */
export const FIRST_CHECK_DELAY_MS = 5000;
/** How often a running app asks itself whether a day has passed. */
export const TICK_MS = 60 * 60 * 1000;

export interface Release {
  version: string;
  url: string;
}

export interface UpdateState {
  /** When the last check ran (ms), successful or not: at most one request a day. */
  lastCheck?: number;
  /** The latest release the last successful check found. */
  latest?: Release;
  /** "Skip this version": never offered again; a later one is. */
  skipped?: string;
  /** "Remind me tomorrow": hidden until then. */
  remindAfter?: number;
}

/** "1.2.0" / "v1.2.0" -> [1, 2, 0]; anything else -> null (as native/src/app/UpdateCheck.cpp). */
export function parseVersion(text: string): [number, number, number] | null {
  const m = /^[vV]?(\d{1,5})\.(\d{1,5})\.(\d{1,5})$/.exec(text);
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}

/** `latest` is newer than `current`; false when either is not a version. */
export function isNewer(latest: string, current: string): boolean {
  const a = parseVersion(latest);
  const b = parseVersion(current);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
  return false;
}

/** A day has passed since the last check (or there was none, or the clock went back). */
export function checkDue(state: UpdateState, now: number): boolean {
  return state.lastCheck === undefined || now - state.lastCheck >= DAY_MS || now < state.lastCheck;
}

/** The release to offer in the title bar, or null: newer than this version, not skipped, not put off until later. */
export function offered(state: UpdateState, current: string, now: number): Release | null {
  const l = state.latest;
  if (!l || !isNewer(l.version, current)) return null;
  if (state.skipped === l.version) return null;
  if (state.remindAfter !== undefined && now < state.remindAfter) return null;
  return l;
}

/** Only release pages of the project are opened (the native side builds them; this is the second lock). */
export function isReleasePage(url: string): boolean {
  return /^https:\/\/github\.com\/f1resh\/sshtermaq\/releases\/tag\/[vV]?\d{1,5}\.\d{1,5}\.\d{1,5}$/.test(url);
}

/** The command that upgrades this copy, if it has one. */
export function upgradeCommand(channel: string): string | null {
  return channel === 'homebrew' ? 'brew upgrade --cask sshtermaq' : null;
}
