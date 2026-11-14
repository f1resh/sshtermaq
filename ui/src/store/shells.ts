/**
 * The shells installed on this computer (shells:list), for "New local terminal": loaded once, on first use.
 */
import { create } from 'zustand';
import { api } from '@/bridge/api';
import type { LocalShellInfo } from '@shared/types';

interface ShellsState {
  /** null until loaded. Empty: this computer cannot host local terminals (Windows 10 before 1809). */
  shells: LocalShellInfo[] | null;
  load(): Promise<LocalShellInfo[]>;
}

let loading: Promise<LocalShellInfo[]> | null = null;

export const useShells = create<ShellsState>((set, get) => ({
  shells: null,
  load() {
    const have = get().shells;
    if (have) return Promise.resolve(have);
    loading ??= api.shells
      .list()
      .catch(() => [] as LocalShellInfo[])
      .then((shells) => {
        set({ shells });
        return shells;
      })
      .finally(() => {
        loading = null;
      });
    return loading;
  }
}));

/** The shell to open by default: settings' choice when it is installed, else the system's default, else the first. */
export function pickDefaultShell(shells: LocalShellInfo[], preferredId: string): LocalShellInfo | null {
  return shells.find((s) => s.id === preferredId) ?? shells.find((s) => s.isDefault) ?? shells[0] ?? null;
}
