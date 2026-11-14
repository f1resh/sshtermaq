/**
 * Quick connect to a local terminal: "local:<shell or command line>" (or the Local chip with the same text). The text is
 * a shell's name or id ("pwsh", "git bash", "ubuntu"), else a command line; empty means the default shell.
 */
import type { LocalShellInfo } from '@shared/types';

export interface LocalTarget {
  /** The installed shell it names, if any. */
  shell: LocalShellInfo | null;
  /** The command line to run when no installed shell matches ('' = the default shell). */
  command: string;
}

/** "local:pwsh" → "pwsh"; null when `text` is not a local target. */
export function localPart(text: string): string | null {
  const m = /^local:\s*(.*)$/is.exec(text.trim());
  return m ? m[1].trim() : null;
}

/** The shell `input` names (exact id or name, then a name or id starting with it), else `input` as a command line. */
export function resolveLocalTarget(input: string, shells: readonly LocalShellInfo[], defaultShell: LocalShellInfo | null): LocalTarget {
  const q = input.trim().toLowerCase();
  if (!q) return { shell: defaultShell, command: '' };
  const exact = shells.find((s) => s.id.toLowerCase() === q || s.name.toLowerCase() === q);
  const prefix = exact ?? shells.find((s) => s.name.toLowerCase().startsWith(q) || s.id.toLowerCase().startsWith(q));
  return prefix ? { shell: prefix, command: '' } : { shell: null, command: input.trim() };
}

/** What opens: an installed shell as is, or the typed command line as a one-off shell. */
export function shellToOpen(t: LocalTarget): LocalShellInfo | null {
  if (t.shell) return t.shell;
  if (!t.command) return null;
  return { id: 'custom', name: t.command, command: t.command, environment: [], isDefault: false };
}
