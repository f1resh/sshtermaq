import { describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import type { VaultImportResult, VaultKey, VaultLogin } from '@shared/types';
import { switchProtocol } from '../session/sessionModel';
import { importSummary, keyLabel, keysAndLogins, loginNameTaken, loginSummary, loginsUsingKey, nameTaken, sessionsUsingKey, sessionsUsingLogin, shortKeyType, suggestedKeyName } from './vaultModel';

const key = (over: Partial<VaultKey> = {}): VaultKey => ({
  id: 'k1',
  name: 'work-laptop',
  kind: 'stored',
  path: '',
  format: 'openssh',
  keyType: 'ssh-ed25519',
  fingerprint: 'SHA256:abc',
  publicKeyOpenSsh: '',
  comment: '',
  encrypted: false,
  createdAt: 0,
  updatedAt: 0,
  ...over
});

describe('Credentials helpers', () => {
  it('finds the sessions that use a key, sorted by name', () => {
    const a = createDefaultSession({ name: 'web-02' });
    a.ssh.auth.keyId = 'k1';
    const b = createDefaultSession({ name: 'web-01' });
    b.ssh.auth.keyId = 'k1';
    const c = createDefaultSession({ name: 'db' });
    c.ssh.auth.privateKeyPath = 'C:/keys/k1';  // a key file of its own: not a Credentials key
    expect(sessionsUsingKey('k1', [a, b, c]).map((s) => s.name)).toEqual(['web-01', 'web-02']);
    expect(sessionsUsingKey('other', [a, b, c])).toEqual([]);
  });

  it('short key type names', () => {
    expect(shortKeyType('ssh-ed25519')).toBe('ed25519');
    expect(shortKeyType('ssh-rsa')).toBe('rsa');
    expect(shortKeyType('ecdsa-sha2-nistp384')).toBe('ecdsa 384');
    expect(shortKeyType('sk-ssh-ed25519@openssh.com')).toBe('sk-ssh-ed25519');
    expect(shortKeyType('')).toBe('unknown type');
  });

  it('the dropdown label marks a missing file', () => {
    expect(keyLabel(key())).toBe('work-laptop · ed25519');
    expect(keyLabel(key({ kind: 'file', path: 'x', missing: true }))).toBe('work-laptop · ed25519 (file missing)');
  });

  it('a new key is offered its comment, else the file name, else "pasted key"', () => {
    expect(suggestedKeyName({ comment: ' me@laptop ' })).toBe('me@laptop');
    expect(suggestedKeyName({ comment: '' }, 'C:\\Users\\me\\.ssh\\router.ppk')).toBe('router.ppk');
    expect(suggestedKeyName({ comment: '' }, '/Users/me/.ssh/id_ed25519')).toBe('id_ed25519');
    expect(suggestedKeyName(null)).toBe('pasted key');
  });

  it('names are unique whatever the case, except for the key being renamed', () => {
    const keys = [key(), key({ id: 'k2', name: 'router' })];
    expect(nameTaken(' WORK-LAPTOP ', keys)).toBe(true);
    expect(nameTaken('work-laptop', keys, 'k1')).toBe(false);
    expect(nameTaken('new', keys)).toBe(false);
  });
});

const login = (over: Partial<VaultLogin> = {}): VaultLogin => ({
  id: 'l1',
  name: 'deploy@prod',
  username: 'deploy',
  password: 'stored',
  keyId: '',
  useAgent: false,
  createdAt: 0,
  updatedAt: 0,
  ...over
});

describe('Credentials logins', () => {
  it('says how a login signs in', () => {
    const keys = [key()];
    expect(loginSummary(login(), keys)).toBe('password stored');
    expect(loginSummary(login({ password: 'ask', keyId: 'k1', useAgent: true }), keys)).toBe('password asked · key work-laptop · agent');
    expect(loginSummary(login({ password: 'none', keyId: 'gone' }), keys)).toBe('key missing');
    expect(loginSummary(login({ password: 'none' }), keys)).toBe('no sign-in method');
  });

  it('finds the sessions that use a login and the logins that use a key', () => {
    const a = createDefaultSession({ name: 'web-01' });
    a.ssh.auth.loginId = 'l1';
    const b = createDefaultSession({ name: 'db' });
    expect(sessionsUsingLogin('l1', [a, b]).map((s) => s.name)).toEqual(['web-01']);
    // S3 sessions that take their access key from the login count too; one that signs in another way does not.
    const s3 = switchProtocol(createDefaultSession({ name: 'backups' }), 's3');
    s3.s3.auth.loginId = 'l1';
    const profile = switchProtocol(createDefaultSession({ name: 'archive' }), 's3');
    profile.s3.auth = { ...profile.s3.auth, method: 'profile', loginId: 'l1' };
    expect(sessionsUsingLogin('l1', [a, b, s3, profile]).map((s) => s.name)).toEqual(['backups', 'web-01']);
    expect(loginsUsingKey('k1', [login({ keyId: 'k1', name: 'b' }), login({ id: 'l2', keyId: 'k1', name: 'a' }), login({ id: 'l3' })]).map((l) => l.name)).toEqual(['a', 'b']);
  });

  it('login names are unique among logins, whatever the case', () => {
    const logins = [login(), login({ id: 'l2', name: 'admin@lab' })];
    expect(loginNameTaken('DEPLOY@PROD', logins)).toBe(true);
    expect(loginNameTaken('deploy@prod', logins, 'l1')).toBe(false);
  });
});

describe('Credentials export and import', () => {
  const result = (over: Partial<VaultImportResult> = {}): VaultImportResult => ({
    path: 'C:/backup.sshtcred',
    keysAdded: 0,
    keysSkipped: 0,
    loginsAdded: 0,
    loginsSkipped: 0,
    secrets: 0,
    renamed: [],
    ...over
  });

  it('counts keys and logins', () => {
    expect(keysAndLogins(2, 1)).toBe('2 keys and 1 login');
    expect(keysAndLogins(1, 0)).toBe('1 key');
    expect(keysAndLogins(0, 3)).toBe('3 logins');
    expect(keysAndLogins(0, 0)).toBe('');
  });

  it('says what an import added, skipped and renamed', () => {
    expect(importSummary(result({ keysAdded: 2, loginsAdded: 1, secrets: 3, keysSkipped: 1, renamed: ['key work → work (2)'] }))).toEqual([
      'Added 2 keys and 1 login (3 secrets).',
      'Already here, left as they are: 1 key.',
      'Renamed, the name was taken: key work → work (2).'
    ]);
    expect(importSummary(result({ keysSkipped: 2, loginsSkipped: 1 }))).toEqual(['Nothing new: everything in the file is already here.']);
  });
});
