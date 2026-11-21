/**
 * Pure helpers for the import wizard (unit tested): conflict detection against saved sessions, filtering,
 * request building and display strings.
 */
import { DEFAULT_PORTS, normalizeFolder } from '@shared/defaults';
import { isMac } from '@shared/platform';
import type { ImportCandidate, ImportPreview, ImportRequest, ImportSource, SessionConfig } from '@shared/types';

export type ConflictPolicy = ImportRequest['conflict'];

export interface ImportOptions {
  targetFolder: string;
  conflict: ConflictPolicy;
  importHostKeys: boolean;
  useDefaultSettings: boolean;
}

export const SOURCE_LABELS: Record<ImportSource, string> = {
  'putty-registry': isMac ? 'PuTTY (~/.putty)' : 'PuTTY (this PC)',
  'putty-regfile': 'PuTTY .reg file',
  'ssh-config': 'OpenSSH config',
  json: 'JSON backup'
};

export function isPuttySource(source: ImportSource): boolean {
  return source === 'putty-registry' || source === 'putty-regfile';
}

/** Folder imported sessions go to by default ("PuTTY" for PuTTY sources, root otherwise). */
export function defaultTargetFolder(source: ImportSource): string {
  return isPuttySource(source) ? 'PuTTY' : '';
}

export function defaultImportOptions(preview: ImportPreview): ImportOptions {
  return {
    targetFolder: defaultTargetFolder(preview.source),
    conflict: 'rename',
    importHostKeys: preview.hostKeys.length > 0,
    useDefaultSettings: false
  };
}

/** Final folder of a candidate: target folder + the candidate's own folder. */
export function destinationFolder(c: ImportCandidate, targetFolder: string): string {
  return normalizeFolder([targetFolder, c.session.folder].filter(Boolean).join('/'));
}

/** Whether importing `c` under `targetFolder` collides with a saved session (same name + folder, case-insensitive). */
export function candidateExists(c: ImportCandidate, sessions: readonly Pick<SessionConfig, 'name' | 'folder'>[], targetFolder: string): boolean {
  const folder = destinationFolder(c, targetFolder);
  const name = c.name.trim().toLowerCase();
  return sessions.some((s) => s.name.trim().toLowerCase() === name && normalizeFolder(s.folder) === folder);
}

/** "user@host:port" (port only when non-default), serial line for serial sessions. */
export function candidateAddress(s: SessionConfig): string {
  if (s.protocol === 'serial') return `${s.serial.line} @ ${s.serial.baudRate}`;
  const user = s.username ? `${s.username}@` : '';
  const host = s.host.includes(':') ? `[${s.host}]` : s.host;
  const port = s.port && s.port !== DEFAULT_PORTS[s.protocol] ? `:${s.port}` : '';
  return `${user}${host}${port}`;
}

/** Case-insensitive match on name, address and protocol. */
export function filterCandidates(list: readonly ImportCandidate[], query: string): ImportCandidate[] {
  const q = query.trim().toLowerCase();
  if (!q) return [...list];
  return list.filter((c) => `${c.name} ${candidateAddress(c.session)} ${c.session.protocol}`.toLowerCase().includes(q));
}

/** Builds the apply request for the selected candidate keys. */
export function buildImportRequest(preview: ImportPreview, selected: ReadonlySet<string>, opts: ImportOptions): ImportRequest {
  const req: ImportRequest = {
    candidates: preview.candidates.filter((c) => selected.has(c.key)),
    hostKeys: opts.importHostKeys ? preview.hostKeys : [],
    conflict: opts.conflict,
    targetFolder: normalizeFolder(opts.targetFolder)
  };
  if (opts.useDefaultSettings && preview.defaultSettings) req.defaultSettings = preview.defaultSettings;
  return req;
}

/** A command an imported session runs on this computer when it connects (or opens). */
export interface LocalCommand {
  kind: 'proxy' | 'terminal';
  command: string;
}

/** What `s` runs on this computer: a local proxy command (PuTTY's proxy type "Local", OpenSSH's ProxyCommand), or a
 *  local terminal session's own command line. */
export function localCommands(s: SessionConfig): LocalCommand[] {
  const out: LocalCommand[] = [];
  if (s.proxy.type === 'command' && s.proxy.localCommand.trim()) out.push({ kind: 'proxy', command: s.proxy.localCommand.trim() });
  if (s.protocol === 'local' && s.local.command.trim()) out.push({ kind: 'terminal', command: s.local.command.trim() });
  return out;
}

export const LOCAL_COMMAND_LABELS: Record<LocalCommand['kind'], string> = {
  proxy: 'Local proxy command, run when it connects',
  terminal: 'Local terminal command, run when it opens'
};

/** A file (a .reg export, a JSON backup) may come from someone else; this computer's PuTTY and ~/.ssh/config not. */
export function isFileSource(source: ImportSource): boolean {
  return source === 'putty-regfile' || source === 'json';
}

/** Keys selected initially: every candidate, except those from a file that run a command on this computer (they are
 *  ticked on purpose). */
export function initialSelection(preview: ImportPreview): Set<string> {
  const fromFile = isFileSource(preview.source);
  return new Set(preview.candidates.filter((c) => !(fromFile && localCommands(c.session).length)).map((c) => c.key));
}

/** "2 of the 5 selected sessions run a command on this computer" ('' when none of them do). */
export function commandsSummary(candidates: readonly ImportCandidate[], selected: ReadonlySet<string>): string {
  const chosen = candidates.filter((c) => selected.has(c.key));
  const n = chosen.filter((c) => localCommands(c.session).length).length;
  if (!n) return '';
  if (chosen.length === 1) return 'The selected session runs a command on this computer';
  return `${n} of the ${chosen.length} selected sessions run${n === 1 ? 's' : ''} a command on this computer`;
}
