/**
 * Mock handlers for the `keys` namespace (owner: ui-dialogs): fake key generation, inspection and conversion
 * so the keygen dialog and the session editor's key info work in the browser preview.
 *
 * Inspection is driven by the file name: `*.ppk` -> PPK v3 (v2 when the name contains "v2"), `*.pem` -> PEM,
 * anything else -> OpenSSH. Names containing "enc" are passphrase protected (old-style encrypted PEM needs the
 * passphrase even to read the public key; the mock passphrase is anything except "wrong"), names containing
 * "missing" fail as if the file did not exist.
 */
import type { KeyInfo, KeygenRequest, KeygenResult, VaultKey, VaultLogin } from '@shared/types';
import { LOCKED_MESSAGE, mockDb, mockVaultLocked, mockVaultState, persistMock, seedVault, seedVaultLogins, type MockVaultEntry } from './core';
import type { MockContext } from './index';

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function randomB64(len: number): string {
  const bytes = new Uint8Array(len);
  globalThis.crypto.getRandomValues(bytes);
  let out = '';
  for (const b of bytes) out += B64[b & 63];
  return out;
}

/** Deterministic pseudo base64 derived from a string (stable fingerprints per mock file). */
function hashB64(seed: string, len: number): string {
  let h = 2166136261;
  let out = '';
  for (let i = 0; out.length < len; i++) {
    h ^= seed.charCodeAt(i % Math.max(1, seed.length)) + i;
    h = Math.imul(h, 16777619) >>> 0;
    out += B64[h & 63];
  }
  return out;
}

function keyTypeName(type: KeygenRequest['type'], bits?: number): string {
  if (type === 'ed25519') return 'ssh-ed25519';
  if (type === 'rsa') return 'ssh-rsa';
  return `ecdsa-sha2-nistp${bits ?? 256}`;
}

function blobLength(type: KeygenRequest['type'], bits?: number): number {
  if (type === 'ed25519') return 68;
  if (type === 'rsa') return Math.round(((bits ?? 3072) / 8) * 1.37) + 28;
  return bits === 521 ? 172 : bits === 384 ? 136 : 104;
}

function wrap(s: string, width = 64): string {
  return s.replace(new RegExp(`(.{${width}})`, 'g'), '$1\n').trimEnd();
}

const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

function fileName(path: string): string {
  return path.split(/[\\/]/).pop() ?? path;
}

function inspectMock(path: string, passphrase?: string): KeyInfo {
  const name = fileName(path).toLowerCase();
  if (!path.trim() || name.includes('missing')) throw new Error(`Cannot open "${path}": the system cannot find the file specified.`);
  const ppk = name.endsWith('.ppk');
  const pem = name.endsWith('.pem');
  const encrypted = name.includes('enc');
  if (pem && encrypted) {
    if (!passphrase) throw new Error('This key is encrypted; a passphrase is required to read it.');
    if (passphrase === 'wrong') throw new Error('Wrong passphrase.');
  }
  const keyType = name.includes('rsa') ? 'ssh-rsa' : name.includes('ecdsa') ? 'ecdsa-sha2-nistp256' : 'ssh-ed25519';
  const blob = `AAAA${hashB64(path + keyType, keyType === 'ssh-rsa' ? 360 : keyType === 'ssh-ed25519' ? 64 : 100)}`;
  return {
    path,
    format: ppk ? (name.includes('v2') ? 'ppk2' : 'ppk3') : pem ? 'pem' : 'openssh',
    keyType,
    comment: pem ? '' : `${name.replace(/\.(ppk|pem|key)$/, '')}@workstation`,
    encrypted,
    fingerprint: `SHA256:${hashB64(path, 43)}`,
    publicKeyOpenSsh: `${keyType} ${blob} ${pem ? '' : name.replace(/\.(ppk|key)$/, '') + '@workstation'}`.trim()
  };
}

export function registerKeysMocks(ctx: MockContext): void {
  ctx.handle('keys:generate', async (req: KeygenRequest): Promise<KeygenResult> => {
    await delay(req.type === 'rsa' ? 300 + (req.bits ?? 3072) / 6 : 250);
    const kt = keyTypeName(req.type, req.bits);
    const pub = `AAAA${randomB64(blobLength(req.type, req.bits))}`;
    const comment = req.comment || 'mock-key';
    const enc = req.passphrase ? 'aes256-ctr' : 'none';
    const priv = randomB64(req.type === 'rsa' ? 1600 : 380);
    return {
      publicKeyOpenSsh: `${kt} ${pub} ${comment}`,
      fingerprint: `SHA256:${randomB64(43)}`,
      privateKeyOpenSsh: `-----BEGIN OPENSSH PRIVATE KEY-----\n${wrap(`b3BlbnNzaC1rZXktdjEAAAAA${enc === 'none' ? 'BG5vbmU' : 'CmFlczI1Ni1jdHI'}${priv}`, 70)}\n-----END OPENSSH PRIVATE KEY-----\n`,
      privateKeyPpk: [
        `PuTTY-User-Key-File-3: ${kt}`,
        `Encryption: ${req.passphrase ? 'aes256-cbc' : 'none'}`,
        `Comment: ${comment}`,
        `Public-Lines: ${wrap(pub).split('\n').length}`,
        wrap(pub),
        ...(req.passphrase ? ['Key-Derivation: Argon2id', 'Argon2-Memory: 8192', 'Argon2-Passes: 13', 'Argon2-Parallelism: 1', `Argon2-Salt: ${hashB64(pub, 32)}`] : []),
        `Private-Lines: ${wrap(priv).split('\n').length}`,
        wrap(priv),
        `Private-MAC: ${hashB64(priv, 64)}`,
        ''
      ].join('\n')
    };
  });

  ctx.handle('keys:inspect', async (path: string, passphrase?: string) => {
    await delay(60);
    return inspectMock(path, passphrase ?? undefined);
  });

  ctx.handle('keys:convert', async (path: string, passphrase: string, to: 'openssh' | 'ppk') => {
    await delay(120);
    const info = inspectMock(path, passphrase);
    if (info.encrypted && !passphrase) throw new Error('This key is encrypted; enter its passphrase to convert it.');
    if (info.encrypted && passphrase === 'wrong') throw new Error('Wrong passphrase.');
    const [kt, blob, ...rest] = info.publicKeyOpenSsh.split(' ');
    const comment = rest.join(' ');
    const priv = hashB64(path + 'private', kt === 'ssh-rsa' ? 1600 : 380);
    if (to === 'openssh') {
      return `-----BEGIN OPENSSH PRIVATE KEY-----\n${wrap(`b3BlbnNzaC1rZXktdjEAAAAA${priv}`, 70)}\n-----END OPENSSH PRIVATE KEY-----\n`;
    }
    return [
      `PuTTY-User-Key-File-3: ${kt}`,
      `Encryption: ${info.encrypted ? 'aes256-cbc' : 'none'}`,
      `Comment: ${comment}`,
      `Public-Lines: ${wrap(blob).split('\n').length}`,
      wrap(blob),
      `Private-Lines: ${wrap(priv).split('\n').length}`,
      wrap(priv),
      `Private-MAC: ${hashB64(priv, 64)}`,
      ''
    ].join('\n');
  });

  /* ---- Credentials (vault:*), kept in the mock database ---- */
  const vault = () => (mockDb.vault ??= seedVault());
  const changed = () => {
    persistMock();
    ctx.emit('vault:onChanged');
  };
  const listed = (e: MockVaultEntry): VaultKey => ({
    ...e.key,
    missing: e.key.kind === 'file' && fileName(e.key.path).toLowerCase().includes('missing'),
    passphraseRemembered: !!e.passphrase
  });
  const taken = (name: string, exceptId = '') =>
    vault().some((e) => e.key.id !== exceptId && e.key.name.trim().toLowerCase() === name.trim().toLowerCase());
  const entry = (info: KeyInfo, name: string, fallback: string, kind: VaultKey['kind'], path = ''): VaultKey => {
    const n = name.trim() || info.comment.trim() || fallback;
    if (taken(n)) throw new Error(`A key named "${n}" already exists`);
    const now = Date.now();
    return {
      id: `vault-${now}-${Math.round(Math.random() * 1e6)}`,
      name: n,
      kind,
      path,
      format: info.format,
      keyType: info.keyType,
      fingerprint: info.fingerprint,
      publicKeyOpenSsh: info.publicKeyOpenSsh,
      comment: info.comment,
      encrypted: info.encrypted,
      createdAt: now,
      updatedAt: now
    };
  };

  ctx.handle('vault:list', () => [...vault()].map(listed).sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' })));
  ctx.handle('vault:inspectText', async (text: string) => {
    await delay(40);
    return inspectTextMock(text);
  });
  ctx.handle('vault:addKeyText', async (name: string, text: string) => {
    await delay(60);
    if (mockVaultLocked()) throw new Error(LOCKED_MESSAGE);
    const e = { key: entry(inspectTextMock(text), name, 'pasted key', 'stored'), text };
    vault().push(e);
    changed();
    return listed(e);
  });
  ctx.handle('vault:addKeyFile', async (name: string, path?: string) => {
    await delay(60);
    const file = path || (await ctx.invoke<string | null>('dialog:openFile', { title: 'Choose a private key file' }));
    if (!file) return null;
    const e = { key: entry(inspectMock(file), name, fileName(file), 'file', file) };
    vault().push(e);
    changed();
    return listed(e);
  });
  ctx.handle('vault:rename', (id: string, name: string) => {
    const e = vault().find((x) => x.key.id === id);
    if (!e) throw new Error('Key not found');
    if (!name.trim()) throw new Error('The key needs a name');
    if (taken(name, id)) throw new Error(`A key named "${name.trim()}" already exists`);
    e.key = { ...e.key, name: name.trim(), updatedAt: Date.now() };
    changed();
    return listed(e);
  });
  /* The master password (mock: kept as is; the host keeps a derived check value). */
  ctx.handle('vault:status', () => (!mockDb.vaultMaster ? 'none' : mockVaultState.unlocked ? 'unlocked' : 'locked'));
  ctx.handle('vault:setMaster', async (password: string) => {
    await delay(80);
    if (mockDb.vaultMaster) throw new Error('A master password is already set');
    if (password.length < 8) throw new Error('Use at least 8 characters');
    mockDb.vaultMaster = password;
    mockVaultState.unlocked = true;
    changed();
  });
  ctx.handle('vault:unlock', async (password: string) => {
    await delay(80);
    if (!mockDb.vaultMaster) throw new Error('No master password is set');
    if (password !== mockDb.vaultMaster) return false;
    mockVaultState.unlocked = true;
    changed();
    return true;
  });
  ctx.handle('vault:lock', () => {
    if (!mockVaultState.unlocked) return;
    mockVaultState.unlocked = false;
    changed();
  });
  ctx.handle('vault:changeMaster', async (oldPassword: string, newPassword: string) => {
    await delay(80);
    if (!mockDb.vaultMaster) throw new Error('No master password is set');
    if (newPassword.length < 8) throw new Error('Use at least 8 characters');
    if (oldPassword !== mockDb.vaultMaster) throw new Error('The current master password is wrong');
    mockDb.vaultMaster = newPassword;
    mockVaultState.unlocked = true;
    changed();
  });
  ctx.handle('vault:removeMaster', async (password: string) => {
    await delay(80);
    if (!mockDb.vaultMaster) throw new Error('No master password is set');
    if (password !== mockDb.vaultMaster) throw new Error('The master password is wrong');
    delete mockDb.vaultMaster;
    mockVaultState.unlocked = false;
    changed();
  });
  ctx.handle('vault:reset', () => {
    delete mockDb.vaultMaster;
    mockVaultState.unlocked = false;
    mockDb.vault = vault()
      .filter((e) => e.key.kind !== 'stored')
      .map((e) => ({ key: e.key }));
    for (const e of logins()) {
      if (e.login.password === 'stored') e.login = { ...e.login, password: 'ask' };
      delete e.password;
    }
    changed();
  });
  /* Export / import. The mock file is plain JSON with the password in it (the host seals it); import reads what the
     mock save dialog wrote (__mockSaved) at that path. */
  const FORMAT = 'SSHTermaq credentials (mock)';
  ctx.handle('vault:exportFile', async (password: string, includeSecrets: boolean, path?: string) => {
    await delay(80);
    if (password.length < 8) throw new Error("Use at least 8 characters for the file's password");
    if (mockVaultLocked()) throw new Error(LOCKED_MESSAGE);
    let secrets = 0;
    const keys = vault().map((e) => {
      if (e.key.kind === 'stored') secrets++;
      if (includeSecrets && e.passphrase) secrets++;
      return { key: e.key, text: e.text, passphrase: includeSecrets ? e.passphrase : undefined };
    });
    const ls = logins().map((e) => {
      if (includeSecrets && e.password) secrets++;
      return { login: e.login, password: includeSecrets ? e.password : undefined };
    });
    const content = JSON.stringify({ format: FORMAT, password, keys, logins: ls });
    const saved = path ?? (await ctx.invoke<string | null>('dialog:saveTextFile', content, { title: 'Export Credentials', defaultPath: 'credentials.sshtcred' }, true));
    if (!saved) return null;
    return { path: saved, keys: keys.length, logins: ls.length, secrets, skipped: [] };
  });
  ctx.handle('vault:importFile', async (password: string, path?: string) => {
    await delay(80);
    const file = path || (await ctx.invoke<string | null>('dialog:openFile', { title: 'Import Credentials' }));
    if (!file) return null;
    const saved = (globalThis as unknown as { __mockSaved: { path: string; content: string }[] }).__mockSaved.filter((f) => f.path === file).pop();
    if (!saved) throw new Error(`Cannot read ${file}`);
    let data: { format?: string; password?: string; keys: MockVaultEntry[]; logins: { login: VaultLogin; password?: string }[] };
    try {
      data = JSON.parse(saved.content);
    } catch {
      throw new Error('Not a Credentials file');
    }
    if (data.format !== FORMAT) throw new Error('Not a Credentials file');
    if (data.password !== password) throw new Error('Wrong password, or the file is damaged');
    if (mockVaultLocked()) throw new Error(LOCKED_MESSAGE);
    const r = { path: file, keysAdded: 0, keysSkipped: 0, loginsAdded: 0, loginsSkipped: 0, secrets: 0, renamed: [] as string[] };
    const free = (name: string, isTaken: (n: string) => boolean) => {
      let n = name;
      for (let i = 2; isTaken(n); i++) n = `${name} (${i})`;
      return n;
    };
    for (const e of data.keys) {
      if (vault().some((x) => x.key.id === e.key.id || (!!e.key.fingerprint && x.key.fingerprint === e.key.fingerprint))) {
        r.keysSkipped++;
        continue;
      }
      const name = free(e.key.name, (n) => taken(n));
      if (name !== e.key.name) r.renamed.push(`key ${e.key.name} → ${name}`);
      vault().push({ key: { ...e.key, name }, text: e.text, passphrase: e.passphrase });
      r.secrets += (e.key.kind === 'stored' ? 1 : 0) + (e.passphrase ? 1 : 0);
      r.keysAdded++;
    }
    for (const e of data.logins) {
      if (logins().some((x) => x.login.id === e.login.id)) {
        r.loginsSkipped++;
        continue;
      }
      const name = free(e.login.name, (n) => loginTaken(n));
      if (name !== e.login.name) r.renamed.push(`login ${e.login.name} → ${name}`);
      const asks = e.login.password === 'stored' && !e.password;
      logins().push({ login: { ...e.login, name, password: asks ? 'ask' : e.login.password }, password: e.password });
      if (e.password) r.secrets++;
      r.loginsAdded++;
    }
    if (r.keysAdded || r.loginsAdded) changed();
    return r;
  });
  ctx.handle('vault:forgetPassphrase', (id: string) => {
    const e = vault().find((x) => x.key.id === id);
    if (!e?.passphrase) return;
    delete e.passphrase;
    changed();
  });
  ctx.handle('vault:delete', (id: string) => {
    mockDb.vault = vault().filter((e) => e.key.id !== id);
    changed();
  });
  const logins = () => (mockDb.vaultLogins ??= seedVaultLogins());
  const loginTaken = (name: string, exceptId = '') =>
    logins().some((e) => e.login.id !== exceptId && e.login.name.trim().toLowerCase() === name.trim().toLowerCase());
  const checkLogin = (l: VaultLogin) => {
    if (!l.name.trim()) throw new Error('The login needs a name');
    if (!l.username.trim()) throw new Error('The login needs a user name');
  };
  ctx.handle('vault:listLogins', () => logins().map((e) => e.login).sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' })));
  ctx.handle('vault:addLogin', (l: VaultLogin, password?: string | null) => {
    checkLogin(l);
    if (loginTaken(l.name)) throw new Error(`A login named "${l.name.trim()}" already exists`);
    if (l.password === 'stored' && !password) throw new Error('Enter the password to store');
    if (l.password === 'stored' && mockVaultLocked()) throw new Error(LOCKED_MESSAGE);
    const now = Date.now();
    const login: VaultLogin = { ...l, id: `login-${now}-${Math.round(Math.random() * 1e6)}`, name: l.name.trim(), username: l.username.trim(), createdAt: now, updatedAt: now };
    logins().push({ login, password: l.password === 'stored' ? (password ?? undefined) : undefined });
    changed();
    return login;
  });
  ctx.handle('vault:updateLogin', (l: VaultLogin, password?: string | null) => {
    checkLogin(l);
    const e = logins().find((x) => x.login.id === l.id);
    if (!e) throw new Error('Login not found');
    if (loginTaken(l.name, l.id)) throw new Error(`A login named "${l.name.trim()}" already exists`);
    if (l.password === 'stored') {
      if (password && mockVaultLocked()) throw new Error(LOCKED_MESSAGE);
      if (password) e.password = password;
      else if (!e.password) throw new Error('Enter the password to store');
    } else e.password = undefined;
    e.login = { ...l, name: l.name.trim(), username: l.username.trim(), createdAt: e.login.createdAt, updatedAt: Date.now() };
    changed();
    return e.login;
  });
  ctx.handle('vault:deleteLogin', (id: string) => {
    mockDb.vaultLogins = logins().filter((e) => e.login.id !== id);
    changed();
  });
  ctx.handle('vault:exportKey', async (id: string) => {
    const e = vault().find((x) => x.key.id === id);
    if (!e) throw new Error('Key not found');
    if (e.key.kind !== 'stored') throw new Error('This key is a linked file: it is already a file');
    return ctx.invoke<string | null>('dialog:saveTextFile', e.text ?? '', { title: `Export the key "${e.key.name}"`, defaultPath: e.key.name }, true);
  });
}

/** What pasted text is, as the host's keys::inspectKeyText says it (format from the header lines). */
function inspectTextMock(text: string): KeyInfo {
  const t = text.trim();
  if (/^(ssh-|ecdsa-|sk-)/.test(t) || t.includes('PUBLIC KEY')) throw new Error('This is a public key: paste the private key');
  const ppk = /^PuTTY-User-Key-File-(\d):\s*(\S+)/m.exec(t);
  const format: KeyInfo['format'] = ppk ? (ppk[1] === '2' ? 'ppk2' : 'ppk3') : t.includes('BEGIN OPENSSH PRIVATE KEY') ? 'openssh' : /BEGIN (RSA |EC |DSA )?PRIVATE KEY/.test(t) ? 'pem' : 'unknown';
  if (format === 'unknown') throw new Error('Not a recognised private key file');
  const keyType = ppk ? ppk[2] : /RSA PRIVATE/.test(t) ? 'ssh-rsa' : 'ssh-ed25519';
  const comment = /^Comment:\s*(.*)$/m.exec(t)?.[1]?.trim() ?? '';
  const encrypted = ppk ? !/^Encryption:\s*none/m.test(t) : t.includes('ENCRYPTED') || t.includes('CmFlczI1Ni1jdHI');
  const blob = `AAAA${hashB64(t, keyType === 'ssh-rsa' ? 360 : 64)}`;
  return { path: '', format, keyType, comment, encrypted, fingerprint: `SHA256:${hashB64(t, 43)}`, publicKeyOpenSsh: `${keyType} ${blob}${comment ? ` ${comment}` : ''}` };
}
