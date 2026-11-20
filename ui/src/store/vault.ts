/**
 * Credentials as the UI knows them: the public part of keys and logins (vault:list / vault:listLogins; a stored
 * key's text and passwords never come to the UI) and the master password status. Loaded on first use and reloaded on
 * vault:onChanged.
 *
 * Unlocking: `requestUnlock()` shows the "Unlock Credentials" dialog (UnlockHost, mounted once at the app root) and
 * resolves true once unlocked; `withUnlock(fn)` runs fn and, when the host answers "Credentials are locked", asks to
 * unlock and runs it again.
 */
import { useEffect } from 'react';
import { create } from 'zustand';
import { api } from '@/bridge/api';
import type { VaultKey, VaultLogin, VaultStatus } from '@shared/types';

interface VaultState {
  keys: VaultKey[];
  logins: VaultLogin[];
  status: VaultStatus;
  loaded: boolean;
  reload(): Promise<void>;
  /** The pending unlock request (the dialog is open), resolved with true once unlocked. */
  unlockRequest: ((unlocked: boolean) => void) | null;
}

export const useVault = create<VaultState>((set) => ({
  keys: [],
  logins: [],
  status: 'none',
  loaded: false,
  unlockRequest: null,
  async reload() {
    try {
      const [keys, logins, status] = await Promise.all([api.vault.list(), api.vault.listLogins(), api.vault.status()]);
      set({
        keys: Array.isArray(keys) ? keys : [],
        logins: Array.isArray(logins) ? logins : [],
        status: status === 'locked' || status === 'unlocked' ? status : 'none',
        loaded: true
      });
    } catch {
      set({ loaded: true });
    }
  }
}));

let subscribed = false;

/** Starts loading once and follows changes (idempotent). */
export function initVault(): void {
  if (subscribed) return;
  subscribed = true;
  api.vault.onChanged(() => void useVault.getState().reload());
  void useVault.getState().reload();
}

/** Credentials' keys, sorted by name; loads them on first use. */
export function useVaultKeys(): VaultKey[] {
  useEffect(initVault, []);
  return useVault((s) => s.keys);
}

/** Credentials' logins, sorted by name; loads them on first use. */
export function useVaultLogins(): VaultLogin[] {
  useEffect(initVault, []);
  return useVault((s) => s.logins);
}

/** The master password status: 'none', 'locked' or 'unlocked'. */
export function useVaultStatus(): VaultStatus {
  useEffect(initVault, []);
  return useVault((s) => s.status);
}

/** Opens the unlock dialog (one at a time); true once unlocked, false when cancelled. */
export function requestUnlock(): Promise<boolean> {
  const pending = useVault.getState().unlockRequest;
  return new Promise((resolve) => {
    useVault.setState({
      unlockRequest: (ok) => {
        pending?.(ok);
        resolve(ok);
      }
    });
  });
}

/** Closes the unlock dialog with its answer (UnlockHost). */
export function finishUnlock(unlocked: boolean): void {
  const done = useVault.getState().unlockRequest;
  useVault.setState({ unlockRequest: null });
  done?.(unlocked);
}

export const isLockedError = (e: unknown): boolean => /Credentials are locked/.test(e instanceof Error ? e.message : String(e));

/** Runs `fn`; when Credentials are locked, asks to unlock them and runs it once more (else rethrows). */
export async function withUnlock<T>(fn: () => Promise<T>): Promise<T> {
  try {
    return await fn();
  } catch (e) {
    if (!isLockedError(e) || !(await requestUnlock())) throw e;
    return fn();
  }
}
