/**
 * Pure helpers for Credentials (unit tested in vaultModel.test.ts): which saved sessions use a key, short labels for
 * key types, and the name a new key is offered.
 */
import type { KeyInfo, SessionConfig, VaultImportResult, VaultKey, VaultLogin } from '@shared/types';

/** The saved sessions that use this Credentials key (ssh.auth.keyId), sorted by name. */
export function sessionsUsingKey(keyId: string, sessions: readonly SessionConfig[]): SessionConfig[] {
  return sessions.filter((s) => s.ssh?.auth?.keyId === keyId).sort((a, b) => a.name.localeCompare(b.name));
}

/** The saved sessions that sign in with this login, sorted by name: SSH sessions by ssh.auth.loginId, S3 sessions by
 *  s3.auth.loginId when they sign in with access keys (a login's user name is the access key ID). */
export function sessionsUsingLogin(loginId: string, sessions: readonly SessionConfig[]): SessionConfig[] {
  return sessions.filter((s) => usesLogin(s, loginId)).sort((a, b) => a.name.localeCompare(b.name));
}

function usesLogin(s: SessionConfig, loginId: string): boolean {
  if (s.protocol === 's3') return s.s3?.auth?.method === 'keys' && s.s3.auth.loginId === loginId;
  return s.ssh?.auth?.loginId === loginId;
}

/** The logins that use this key, sorted by name. */
export function loginsUsingKey(keyId: string, logins: readonly VaultLogin[]): VaultLogin[] {
  return logins.filter((l) => l.keyId === keyId).sort((a, b) => a.name.localeCompare(b.name));
}

/**
 * How a login signs in, in words: "password stored · key work-laptop · agent". A key that is no longer in Credentials
 * reads "key missing"; nothing at all reads "no sign-in method".
 */
export function loginSummary(l: Pick<VaultLogin, 'password' | 'keyId' | 'useAgent'>, keys: readonly VaultKey[]): string {
  const parts: string[] = [];
  if (l.password === 'stored') parts.push('password stored');
  if (l.password === 'ask') parts.push('password asked');
  if (l.keyId) {
    const k = keys.find((x) => x.id === l.keyId);
    parts.push(k ? `key ${k.name}` : 'key missing');
  }
  if (l.useAgent) parts.push('agent');
  return parts.length ? parts.join(' · ') : 'no sign-in method';
}

/** A login name already used by another login (names are unique among logins, whatever the case). */
export function loginNameTaken(name: string, logins: readonly VaultLogin[], exceptId = ''): boolean {
  const n = name.trim().toLowerCase();
  return logins.some((l) => l.id !== exceptId && l.name.trim().toLowerCase() === n);
}

/** "ssh-ed25519" -> "ed25519", "ssh-rsa" -> "rsa", "ecdsa-sha2-nistp256" -> "ecdsa 256", "" -> "unknown type". */
export function shortKeyType(keyType: string): string {
  if (!keyType) return 'unknown type';
  const ecdsa = /^ecdsa-sha2-nistp(\d+)/.exec(keyType);
  if (ecdsa) return `ecdsa ${ecdsa[1]}`;
  return keyType.replace(/^ssh-/, '').replace(/@openssh\.com$/, '');
}

/** "work-laptop · ed25519" (with "(file missing)" when a linked file is gone): the label in a key dropdown. */
export function keyLabel(k: VaultKey): string {
  return `${k.name} · ${shortKeyType(k.keyType)}${k.missing ? ' (file missing)' : ''}`;
}

/** The name a new key is offered: its comment, else the file name (a linked file), else "pasted key". */
export function suggestedKeyName(info: Pick<KeyInfo, 'comment'> | null, path = ''): string {
  const comment = info?.comment?.trim();
  if (comment) return comment;
  const file = path.split(/[\\/]/).pop()?.trim();
  return file || 'pasted key';
}

/** A name already used by another key (names are unique, whatever the case). */
export function nameTaken(name: string, keys: readonly VaultKey[], exceptId = ''): boolean {
  const n = name.trim().toLowerCase();
  return keys.some((k) => k.id !== exceptId && k.name.trim().toLowerCase() === n);
}

const count = (n: number, what: string) => `${n} ${what}${n === 1 ? '' : 's'}`;

/** "2 keys and 1 login", "1 key", "" when both are 0. */
export function keysAndLogins(keys: number, logins: number): string {
  return [keys ? count(keys, 'key') : '', logins ? count(logins, 'login') : ''].filter(Boolean).join(' and ');
}

/** What an import did, in a few lines for the import dialog. */
export function importSummary(r: VaultImportResult): string[] {
  const added = keysAndLogins(r.keysAdded, r.loginsAdded);
  const here = keysAndLogins(r.keysSkipped, r.loginsSkipped);
  const lines = [added ? `Added ${added}${r.secrets ? ` (${count(r.secrets, 'secret')})` : ''}.` : 'Nothing new: everything in the file is already here.'];
  if (added && here) lines.push(`Already here, left as they are: ${here}.`);
  if (r.renamed.length) lines.push(`Renamed, the name was taken: ${r.renamed.join('; ')}.`);
  return lines;
}
