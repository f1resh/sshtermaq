import { createDefaultSession, createDefaultSettings, deepMerge, normalizeFolder, normalizeSession } from '@shared/defaults';
import { isMac } from '@shared/platform';
import type { AppSettings, KnownHost, SessionConfig, VaultKey, VaultLogin } from '@shared/types';
import type { MockContext } from './index';

const LS_KEY = 'sshtermaq.mock.v1';

interface MockDb {
  sessions: SessionConfig[];
  folders: string[];
  settings: AppSettings;
  knownHosts: KnownHost[];
  state: Record<string, unknown>;
  passwords: Record<string, string>;
  /** Credentials' keys; a stored key's text and a remembered passphrase beside it (the real host keeps both
   *  encrypted). Absent in older mock data. */
  vault?: MockVaultEntry[];
  /** Credentials' logins; a stored password beside it. */
  vaultLogins?: { login: VaultLogin; password?: string }[];
  /** The master password of Credentials (the real host keeps only a derived check value). */
  vaultMaster?: string;
}

export interface MockVaultEntry {
  key: VaultKey;
  text?: string;
  passphrase?: string;
}

function seed(): MockDb {
  const mk = (o: Partial<SessionConfig>) => createDefaultSession(o);
  return {
    sessions: [
      mk({ name: 'web-01', folder: 'Production/Web', host: 'web-01.example.com', username: 'deploy', color: '#e5534b' }),
      mk({ name: 'web-02', folder: 'Production/Web', host: 'web-02.example.com', username: 'deploy', color: '#e5534b' }),
      mk({ name: 'db-primary', folder: 'Production', host: '10.0.0.12', username: 'postgres' }),
      mk({ name: 'staging', folder: 'Staging', host: 'staging.example.com', username: 'ubuntu', color: '#3fb950' }),
      mk({ name: 'router', protocol: 'telnet', port: 23, host: '192.168.1.1' }),
      mk({ name: 'Console COM3', protocol: 'serial', port: 0, host: '', serial: { line: 'COM3', baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' } }),
      mk({ name: 'raspberrypi', host: 'raspberrypi.local', username: 'pi' }),
      mk({
        name: 'media (S3)',
        protocol: 's3',
        port: 0,
        host: '',
        openAs: 'files',
        s3: { ...createDefaultSession().s3, auth: { method: 'keys', accessKeyId: 'AKIAMOCKEXAMPLE0001', loginId: '', profile: '' } }
      })
    ],
    folders: ['Production', 'Production/Web', 'Staging'],
    settings: createDefaultSettings(),
    knownHosts: [],
    state: {},
    passwords: {},
    vault: seedVault(),
    vaultLogins: seedVaultLogins()
  };
}

/** One login so the page has one: deploy, stored password, the seeded "work-laptop" key. */
export function seedVaultLogins(): NonNullable<MockDb['vaultLogins']> {
  const now = Date.now() - 86_400_000;
  return [
    {
      login: { id: 'login-deploy', name: 'deploy@prod', username: 'deploy', password: 'stored', keyId: 'vault-work-laptop', useAgent: false, createdAt: now, updatedAt: now },
      password: 'secret'
    }
  ];
}

/** Two keys so the Credentials page has something: one stored (pasted earlier), one linked file. */
export function seedVault(): NonNullable<MockDb['vault']> {
  const now = Date.now() - 86_400_000;
  const base = { format: 'openssh' as const, encrypted: false, createdAt: now, updatedAt: now };
  return [
    {
      key: {
        ...base,
        id: 'vault-work-laptop',
        name: 'work-laptop',
        kind: 'stored',
        path: '',
        keyType: 'ssh-ed25519',
        fingerprint: 'SHA256:Kx7mQ2vZp1d9YtLrN4sWc8bE3uHf6gJaO0iTqXyVnRk',
        publicKeyOpenSsh: 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMockWorkLaptopKeyForTheDemoOnly0000 me@work-laptop',
        comment: 'me@work-laptop'
      },
      text: '-----BEGIN OPENSSH PRIVATE KEY-----\nmock\n-----END OPENSSH PRIVATE KEY-----\n'
    },
    {
      key: {
        ...base,
        id: 'vault-router',
        name: 'router',
        kind: 'file',
        path: isMac ? '/Users/demo/.ssh/router.ppk' : 'C:\\Users\\demo\\.ssh\\router.ppk',
        format: 'ppk3',
        keyType: 'ssh-rsa',
        fingerprint: 'SHA256:Zt2pQ8wLmN5vB1cX7kJ3hG9fD4sA6eR0yUiOtPqWnMk',
        publicKeyOpenSsh: 'ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQMockRouterKeyForTheDemoOnly admin@router',
        comment: 'admin@router',
        encrypted: true
      },
      passphrase: 'demo'
    }
  ];
}

/** Credentials are locked after each start (page load) when a master password is set, as in the app. */
export const mockVaultState = { unlocked: false };
export const mockVaultLocked = (): boolean => !!mockDb.vaultMaster && !mockVaultState.unlocked;
export const LOCKED_MESSAGE = 'Credentials are locked: unlock them with the master password first';

/** Whether a mock SSH connection needs a sealed secret: its stored key, the key's remembered passphrase, or its login's stored password. */
export function mockNeedsUnlock(s: SessionConfig): boolean {
  if (s.protocol !== 'ssh' || !mockVaultLocked()) return false;
  const loginId = s.ssh.auth.loginId;
  const login = loginId ? mockDb.vaultLogins?.find((e) => e.login.id === loginId) : undefined;
  if (login?.login.password === 'stored') return true;
  const keyId = loginId ? login?.login.keyId : s.ssh.auth.keyId;
  const key = keyId ? mockDb.vault?.find((x) => x.key.id === keyId) : undefined;
  return !!key && (key.key.kind === 'stored' || !!key.passphrase);
}

/**
 * The Credentials key a mock SSH connection signs in with (its login's key, else its own keyId), when that key is
 * passphrase protected and has no remembered passphrase: the mock connection asks for it, as the real one does.
 */
export function mockKeyToUnlock(s: SessionConfig): MockVaultEntry | undefined {
  if (s.protocol !== 'ssh') return undefined;
  const loginId = s.ssh.auth.loginId;
  const keyId = loginId ? mockDb.vaultLogins?.find((e) => e.login.id === loginId)?.login.keyId : s.ssh.auth.keyId;
  const e = keyId ? mockDb.vault?.find((x) => x.key.id === keyId) : undefined;
  return e && e.key.encrypted && !e.passphrase ? e : undefined;
}

function load(): MockDb {
  try {
    const raw = globalThis.localStorage?.getItem(LS_KEY);
    if (raw) return JSON.parse(raw) as MockDb;
  } catch {
    /* ignore */
  }
  return seed();
}

export const mockDb: MockDb = load();

export function persistMock(): void {
  try {
    globalThis.localStorage?.setItem(LS_KEY, JSON.stringify(mockDb));
  } catch {
    /* ignore */
  }
}

function allFolders(): string[] {
  const set = new Set<string>();
  const add = (f: string) => {
    const parts = normalizeFolder(f).split('/').filter(Boolean);
    for (let i = 1; i <= parts.length; i++) set.add(parts.slice(0, i).join('/'));
  };
  mockDb.folders.forEach(add);
  mockDb.sessions.forEach((s) => add(s.folder));
  return [...set].sort((a, b) => a.localeCompare(b));
}

export function registerCoreMocks(ctx: MockContext): void {
  const changed = () => {
    persistMock();
    ctx.emit('sessions:onChanged');
  };

  /* sessions */
  ctx.handle('sessions:list', () => mockDb.sessions);
  ctx.handle('sessions:save', (s: SessionConfig) => {
    const n = normalizeSession(s);
    n.updatedAt = Date.now();
    const i = mockDb.sessions.findIndex((x) => x.id === n.id);
    if (i >= 0) mockDb.sessions[i] = n;
    else mockDb.sessions.push(n);
    changed();
    return n;
  });
  ctx.handle('sessions:saveMany', (list: SessionConfig[]) => {
    for (const s of list) {
      const n = normalizeSession(s);
      const i = mockDb.sessions.findIndex((x) => x.id === n.id);
      if (i >= 0) mockDb.sessions[i] = n;
      else mockDb.sessions.push(n);
    }
    changed();
  });
  ctx.handle('sessions:delete', (ids: string[]) => {
    mockDb.sessions = mockDb.sessions.filter((s) => !ids.includes(s.id));
    changed();
  });
  ctx.handle('sessions:folders', () => allFolders());
  ctx.handle('sessions:createFolder', (p: string) => {
    mockDb.folders.push(normalizeFolder(p));
    changed();
  });
  ctx.handle('sessions:renameFolder', (from: string, to: string) => {
    const a = normalizeFolder(from);
    const b = normalizeFolder(to);
    const re = (f: string) => (f === a ? b : f.startsWith(a + '/') ? b + f.slice(a.length) : f);
    mockDb.folders = mockDb.folders.map(re);
    mockDb.sessions.forEach((s) => (s.folder = re(s.folder)));
    changed();
  });
  ctx.handle('sessions:deleteFolder', (p: string, deleteSessions: boolean) => {
    const a = normalizeFolder(p);
    const inside = (f: string) => f === a || f.startsWith(a + '/');
    const parent = a.includes('/') ? a.slice(0, a.lastIndexOf('/')) : '';
    mockDb.folders = mockDb.folders.filter((f) => !inside(f));
    if (deleteSessions) mockDb.sessions = mockDb.sessions.filter((s) => !inside(s.folder));
    else mockDb.sessions.forEach((s) => inside(s.folder) && (s.folder = parent + s.folder.slice(a.length).replace(/^\//, parent ? '/' : '')));
    changed();
  });

  /* credentials */
  ctx.handle('credentials:hasPassword', (id: string) => id in mockDb.passwords);
  ctx.handle('credentials:setPassword', (id: string, pw: string) => {
    mockDb.passwords[id] = pw;
    persistMock();
  });
  ctx.handle('credentials:clearPassword', (id: string) => {
    delete mockDb.passwords[id];
    persistMock();
  });

  /* settings */
  ctx.handle('settings:get', () => mockDb.settings);
  ctx.handle('settings:update', (patch: unknown) => {
    mockDb.settings = deepMerge(mockDb.settings, patch);
    persistMock();
    ctx.emit('settings:onChanged', structuredClone(mockDb.settings));
    return mockDb.settings;
  });

  /* known hosts */
  ctx.handle('knownHosts:list', () => mockDb.knownHosts);
  ctx.handle('knownHosts:remove', (ids: string[]) => {
    mockDb.knownHosts = mockDb.knownHosts.filter((k) => !ids.includes(k.id));
    persistMock();
    ctx.emit('knownHosts:onChanged');
  });
  ctx.handle('knownHosts:importOpenSsh', () => 0);

  /* app */
  ctx.handle('app:info', () => ({
    name: 'SSHTermaq',
    version: '0.0.0',
    build: 0,
    commit: 'mock',
    buildTime: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
    electron: '',
    chrome: navigator.userAgent,
    node: '',
    platform: 'browser-mock',
    userDataDir: 'C:\\Users\\mock\\AppData\\Roaming\\SSHTermaq',
    portable: false
  }));
  ctx.handle('app:systemPrefersDark', () => globalThis.matchMedia?.('(prefers-color-scheme: dark)').matches ?? true);
  ctx.handle('app:takeOpenRequests', () => []);
  // The browser has no host that outlives the page: a reload starts fresh.
  ctx.on('app:setReloadSnapshot', () => undefined);
  ctx.handle('app:takeReloadInfo', () => ({ reloaded: false, closedConnections: 0 }));
  ctx.handle('app:getState', (key: string) => mockDb.state[key]);
  ctx.handle('app:setState', (key: string, value: unknown) => {
    mockDb.state[key] = value;
    persistMock();
  });
  ctx.handle('app:readClipboard', async () => {
    try {
      return await navigator.clipboard.readText();
    } catch {
      return '';
    }
  });
  ctx.on('app:writeClipboard', (text: string) => {
    (globalThis as { __mockClipboard?: string }).__mockClipboard = text; // what the Playwright tests read back
    void navigator.clipboard?.writeText(text).catch(() => undefined);
  });
  ctx.handle('app:logDirectory', () => 'C:\\Users\\mock\\AppData\\Roaming\\SSHTermaq\\logs');
  ctx.handle('app:readFileDataUrl', () => null);
  ctx.handle('app:openExternal', (url: string) => void globalThis.open?.(url, '_blank'));
  ctx.handle('app:openPath', (path: string) => void console.info('[mock] open', path));
  ctx.on('app:showItemInFolder', (path: string) => console.info('[mock] show in folder', path));
  ctx.on('app:confirmClose', () => undefined);
  ctx.on('app:quit', () => console.info('[mock] quit'));
  ctx.on('app:log', (level: string, message: string) => console.warn(`[mock] app log (${level})`, message));
  ctx.on('app:setThemeSource', () => undefined);
  ctx.on('app:setBackgroundMaterial', () => undefined);
  ctx.on('app:setUiScale', () => undefined);
  ctx.on('app:beep', () => undefined);
  // The fonts a fresh system has (no JetBrains Mono, Fira Code, ...), so the font picker's "not installed" shows.
  ctx.handle('app:listFonts', () =>
    (isMac
      ? [['Andale Mono', true], ['Arial', false], ['Courier New', true], ['Helvetica', false], ['Menlo', true], ['Monaco', true], ['SF Mono', true], ['Times New Roman', false]]
      : [['Arial', false], ['Calibri', false], ['Cascadia Code', true], ['Cascadia Mono', true], ['Consolas', true], ['Courier New', true], ['Lucida Console', true], ['Segoe UI', false], ['Times New Roman', false]]
    ).map(([family, monospace]) => ({ family, monospace }))
  );
  ctx.on('app:toggleFullscreen', () => {
    if (document.fullscreenElement) void document.exitFullscreen();
    else void document.documentElement.requestFullscreen?.();
  });

  /* window */
  ctx.handle('window:isMaximized', () => false);
  ctx.on('window:minimize', () => undefined);
  ctx.on('window:toggleMaximize', () => undefined);
  ctx.on('window:close', () => undefined);
  ctx.on('window:setTitle', (t: string) => (document.title = t));
  ctx.on('window:flash', () => undefined);
  ctx.on('window:setTitleBarLayout', () => undefined);
  // The macOS menu bar the UI asked for, for the Playwright tests (ui/e2e).
  ctx.on('window:setMenuBar', (menus: unknown) => void ((globalThis as { __mockMenuBar?: unknown }).__mockMenuBar = menus));

  /* dialogs: cancelled, unless a Playwright test (ui/e2e) set the next answer with __mockDialog(kind, answer) */
  const answers = new Map<string, unknown[]>();
  const next = <T>(kind: string, cancelled: T): T => {
    const queue = answers.get(kind);
    return queue?.length ? (queue.shift() as T) : cancelled;
  };
  const saved: { path: string; content: string; secret: boolean }[] = [];
  Object.assign(globalThis, {
    __mockDialog: (kind: string, answer: unknown) => answers.set(kind, [...(answers.get(kind) ?? []), answer]),
    /** What dialog:saveTextFile wrote (path, content, secret), oldest first. */
    __mockSaved: saved
  });
  ctx.handle('dialog:openFile', () => next<string | null>('openFile', null));
  ctx.handle('dialog:openFiles', () => next<string[]>('openFiles', []));
  ctx.handle('dialog:openDirectory', () => next<string | null>('openDirectory', null));
  ctx.handle('dialog:saveFile', () => next<string | null>('saveFile', null));
  ctx.handle('dialog:saveTextFile', (content: string, _opts?: unknown, secret?: boolean) => {
    const path = next<string | null>('saveTextFile', null);
    if (path) saved.push({ path, content, secret: !!secret });
    return path;
  });

  /* local shells */
  ctx.handle('shells:list', () =>
    isMac
      ? [
          { id: '/bin/zsh', name: 'zsh', command: '/bin/zsh -l', environment: [], isDefault: true },
          { id: '/bin/bash', name: 'bash', command: '/bin/bash -l', environment: [], isDefault: false }
        ]
      : [
          { id: 'cmd', name: 'Command Prompt', command: String.raw`"C:\Windows\System32\cmd.exe"`, environment: [], isDefault: false },
          { id: 'powershell', name: 'Windows PowerShell', command: String.raw`"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo`, environment: [], isDefault: false },
          { id: 'pwsh', name: 'PowerShell 7', command: String.raw`"C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo`, environment: [], isDefault: true },
          { id: 'git-bash', name: 'Git Bash', command: String.raw`"C:\Program Files\Git\bin\bash.exe" --login -i`, environment: [], isDefault: false },
          { id: 'wsl:Ubuntu', name: 'Ubuntu (WSL)', command: String.raw`"C:\Windows\System32\wsl.exe" -d Ubuntu --cd ~`, environment: [], isDefault: false }
        ]
  );

  /* serial */
  ctx.handle('serial:listPorts', () => [
    { path: 'COM1', friendlyName: 'Communications Port (COM1)' },
    { path: 'COM3', friendlyName: 'USB Serial Device (COM3)', manufacturer: 'FTDI' }
  ]);
}
