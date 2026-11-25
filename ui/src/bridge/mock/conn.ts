/**
 * Mock connections: a tiny fake shell that echoes input, so terminal UI can be exercised in a browser.
 * An SSH session whose Credentials key is passphrase protected, with no remembered passphrase, is asked for it first
 * (any passphrase but "wrong" opens it; "Remember for this key" remembers it), as the real connection does. Before
 * that, one that needs a sealed secret while Credentials are locked asks for the master password ("Unlock Credentials").
 * The host CHANGED_KEY_HOST presents a changed host key.
 * The host OLD_RSA_HOST speaks only the old SHA-1 RSA (`ssh-rsa`): it connects only with "Allow old SHA-1 RSA" on.
 */
import type { ConnOpenRequest, EventLogEntry, PromptInput, PromptRequest, PromptResponse, SpecialCommandDef } from '@shared/types';
import { sessionLabel } from '@shared/defaults';
import { mockDb, mockKeyToUnlock, mockNeedsUnlock, mockVaultLocked, mockVaultState, persistMock } from './core';
import type { MockContext } from './index';
import { DEMO_COMMANDS, demoBanner, demoCommand, isFullScreen } from './demoShell';

interface MockConn {
  req: ConnOpenRequest;
  line: string;
  log: EventLogEntry[];
  closed: boolean;
  cols: number;
  rows: number;
  promptId?: string;
  /** Accepted a changed host key: its tunnels wait for the special command "tunnels:open". */
  tunnelsHeld?: boolean;
}

/** A host whose key "changed" since it was stored: the mismatch question, then a connection without forwarding. */
export const CHANGED_KEY_HOST = 'changed-key.example.test';
export const OLD_RSA_HOST = 'old-rsa.example.test';

const conns = new Map<string, MockConn>();

/** The protocol of an open mock connection (sftp:open serves S3 connections from a bucket tree). */
export function mockConnProtocol(connId: string): string | undefined {
  return conns.get(connId)?.req.session.protocol;
}
const pendingPrompts = new Map<string, (r: PromptResponse) => void>();

export function registerConnMocks(ctx: MockContext): void {
  const log = (c: MockConn, message: string) => {
    const e: EventLogEntry = { time: Date.now(), level: 'info', message };
    c.log.push(e);
    ctx.emit('conn:onEventLog', c.req.connId, e);
  };
  const out = (c: MockConn, s: string) => ctx.emit('conn:onData', c.req.connId, s);
  const promptOf = (c: MockConn) => {
    const s = c.req.session;
    const host = (s.name || s.host || 'demo').replace(/\s+/g, '-').toLowerCase();
    return `\x1b[1;32m${s.username || 'demo'}@${host}\x1b[0m:\x1b[1;34m~\x1b[0m$ `;
  };

  ctx.handle('conn:open', (req: ConnOpenRequest) => {
    const c: MockConn = { req, line: '', log: [], closed: false, cols: req.cols || 80, rows: req.rows || 24 };
    conns.set(req.connId, c);
    const s = req.session;
    ctx.emit('conn:onStatus', { connId: req.connId, status: 'connecting', message: `Connecting to ${sessionLabel(s)}` });
    if (s.protocol === 's3') {
      // No terminal: sign in and stay connected for the file manager.
      log(c, `S3: endpoint ${s.s3.endpoint || 'AWS'} (mock)`);
      setTimeout(() => {
        if (c.closed) return;
        ctx.emit('conn:onStatus', { connId: req.connId, status: 'authenticating', message: 'Signing in' });
        setTimeout(() => {
          if (c.closed) return;
          ctx.emit('conn:onStatus', { connId: req.connId, status: 'connected', serverInfo: 'S3 | mock storage' });
          log(c, 'S3: signed in (mock)');
        }, 120);
      }, 100);
      return;
    }
    log(c, `Looking up host "${s.host}" (mock)`);
    const connected = () => {
      if (c.closed) return;
      ctx.emit('conn:onStatus', {
        connId: req.connId,
        status: 'connected',
        serverInfo: 'SSH-2.0-MockSSH_1.0, chacha20-poly1305@openssh.com'
      });
      log(c, 'Connected (mock)');
      out(c, demoBanner() + '\r\n' + promptOf(c));
    };
    /** Shows a prompt; `onAnswer` gets the response unless the user cancelled (then the connection ends). */
    const ask = (prompt: PromptInput, onAnswer: (r: PromptResponse) => void) => {
      const id = `prompt-${Date.now()}-${Math.round(Math.random() * 1e6)}`;
      c.promptId = id;
      pendingPrompts.set(id, (r) => {
        c.promptId = undefined;
        if (c.closed) return;
        if (r.cancelled) {
          c.closed = true;
          ctx.emit('conn:onStatus', { connId: req.connId, status: 'disconnected', clean: false, message: 'Authentication cancelled' });
          return;
        }
        onAnswer(r);
      });
      ctx.emit('prompts:onPrompt', { ...prompt, id, connId: req.connId } as PromptRequest);
    };
    const askPassphrase = (message?: string) => {
      const key = mockKeyToUnlock(s);
      if (!key) return void setTimeout(connected, 250);
      ask({ kind: 'passphrase', title: 'Passphrase for key', keyPath: key.key.name, allowSave: true, message }, (r) => {
        const passphrase = r.values?.[0] ?? '';
        if (passphrase === 'wrong') return askPassphrase('Wrong passphrase, try again.');
        if (r.remember && !mockVaultLocked()) {
          key.passphrase = passphrase;
          persistMock();
          ctx.emit('vault:onChanged');
          log(c, `Passphrase remembered for key "${key.key.name}" from Credentials`);
        }
        setTimeout(connected, 100);
      });
    };
    const askUnlock = (message?: string) => {
      if (!mockNeedsUnlock(s)) return askPassphrase();
      log(c, 'Credentials are locked: asking for the master password');
      ask({ kind: 'unlock', title: 'Unlock Credentials', message: message ?? 'Enter the master password of Credentials.' }, (r) => {
        if ((r.values?.[0] ?? '') !== mockDb.vaultMaster) return void setTimeout(() => askUnlock('Wrong master password, try again.'), 150);
        mockVaultState.unlocked = true;
        ctx.emit('vault:onChanged');
        log(c, 'Credentials unlocked');
        askPassphrase();
      });
    };
    // A server whose host key changed (CHANGED_KEY_HOST): asked like the host does; accepted, the connection runs
    // without forwarding, and its tunnels wait for "Open this session's tunnels".
    const askHostKey = () =>
      ask(
        {
          kind: 'hostkey',
          host: s.host,
          port: s.port || 22,
          keyType: 'ssh-ed25519',
          fingerprint: 'SHA256:mockNewKeyMockNewKeyMockNewKeyMockNewKey0',
          fingerprintMd5: 'MD5:00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff',
          status: 'mismatch',
          previous: ['ssh-ed25519 SHA256:mockOldKeyMockOldKeyMockOldKeyMockOldKey00']
        },
        (r) => {
          if (r.choice === 'reject' || !r.choice) {
            c.closed = true;
            ctx.emit('conn:onStatus', { connId: req.connId, status: 'disconnected', clean: true, message: 'Host key not accepted' });
            return;
          }
          c.tunnelsHeld = true;
          log(c, 'The host key changed: no saved password is sent on this connection, and agent forwarding, X11 forwarding and tunnels are off');
          setTimeout(() => {
            connected();
            out(c, "\r\nThe server's host key changed. Tunnels stay off on this connection. To open the tunnels, choose Special Commands > Open this session's tunnels.\r\n" + promptOf(c));
          }, 100);
        }
      );
    setTimeout(() => {
      if (c.closed) return;
      ctx.emit('conn:onStatus', { connId: req.connId, status: 'authenticating' });
      log(c, 'Authenticating (mock)');
      if (s.host === OLD_RSA_HOST && !s.ssh.allowSha1Rsa) {
        // As native/src/ssh/SshClient.cpp says it (kSha1RsaHint).
        c.closed = true;
        log(c, 'kex error : no match for method server host key algo: server [ssh-rsa], client [ssh-ed25519,rsa-sha2-512,rsa-sha2-256]');
        ctx.emit('conn:onStatus', {
          connId: req.connId,
          status: 'error',
          message: `This server only supports the old SHA-1 RSA signatures (ssh-rsa). Turn on "Allow old SHA-1 RSA" in the session's Algorithms settings.`
        });
        return;
      }
      if (s.host === OLD_RSA_HOST) log(c, 'Allowing ssh-rsa (SHA-1) for this session');
      if (s.host === CHANGED_KEY_HOST) askHostKey();
      else askUnlock();
    }, 150);
  });

  ctx.on('conn:write', (connId: string, data: string) => {
    const c = conns.get(connId);
    if (!c || c.closed) return;
    for (const ch of data) {
      if (ch === '\r') {
        const cmd = c.line.trim();
        c.line = '';
        out(c, '\r\n');
        if (cmd === 'exit') {
          c.closed = true;
          out(c, 'logout\r\n');
          ctx.emit('conn:onStatus', { connId, status: 'disconnected', clean: true, exitCode: 0, message: 'Session ended' });
          return;
        }
        const demo = demoCommand(cmd, c.cols, c.rows);
        if (demo !== undefined) out(c, demo);
        else if (cmd === 'help') out(c, `Commands: help, colors, ls, date, exit, ${DEMO_COMMANDS.join(', ')}\r\n`);
        else if (cmd === 'colors') {
          let s = '';
          for (let i = 0; i < 16; i++) s += `\x1b[48;5;${i}m  ${String(i).padStart(2)}  \x1b[0m` + (i === 7 ? '\r\n' : '');
          out(c, s + '\r\n');
        } else if (cmd === 'ls') out(c, '\x1b[1;34mdocuments\x1b[0m  \x1b[1;34mprojects\x1b[0m  notes.txt  \x1b[1;32mrun.sh\x1b[0m\r\n');
        else if (cmd === 'date') out(c, new Date().toString() + '\r\n');
        else if (cmd) out(c, `${cmd}: command not found\r\n`);
        if (!isFullScreen(cmd)) out(c, promptOf(c));
      } else if (ch === '\x7f' || ch === '\b') {
        if (c.line.length) {
          c.line = c.line.slice(0, -1);
          out(c, '\b \b');
        }
      } else if (ch === '\x03') {
        c.line = '';
        out(c, '^C\r\n' + promptOf(c));
      } else if (ch >= ' ') {
        c.line += ch;
        out(c, ch);
      }
    }
  });

  ctx.on('conn:resize', (connId: string, cols: number, rows: number) => {
    const c = conns.get(connId);
    if (c && cols > 0 && rows > 0) {
      c.cols = cols;
      c.rows = rows;
    }
  });
  ctx.on('conn:close', (connId: string) => {
    const c = conns.get(connId);
    if (!c || c.closed) return;
    c.closed = true;
    if (c.promptId) {
      pendingPrompts.delete(c.promptId);
      ctx.emit('prompts:onCancel', c.promptId);
    }
    ctx.emit('conn:onStatus', { connId, status: 'disconnected', clean: true, message: 'Connection closed by user' });
  });
  ctx.on('conn:special', (connId: string, id: string) => {
    const c = conns.get(connId);
    if (!c) return;
    log(c, `Special command: ${id}`);
    if (id === 'tunnels:open' && c.tunnelsHeld) {
      c.tunnelsHeld = false;
      log(c, 'Opening the tunnels (asked for after the host key changed)');
    }
  });
  ctx.handle('conn:specials', (connId: string): SpecialCommandDef[] => [
    ...(conns.get(connId)?.tunnelsHeld ? [{ id: 'tunnels:open', label: "Open this session's tunnels", group: 'Host key changed' }] : []),
    { id: 'break', label: 'Break' },
    { id: 'eof', label: 'EOF' },
    { id: 'ignore', label: 'IGNORE message' },
    { id: 'signal:INT', label: 'SIGINT', group: 'Signals' },
    { id: 'signal:TERM', label: 'SIGTERM', group: 'Signals' },
    { id: 'signal:KILL', label: 'SIGKILL', group: 'Signals' }
  ]);
  ctx.handle('conn:eventLog', (connId: string) => conns.get(connId)?.log ?? []);
  ctx.on('prompts:respond', (id: string, r: PromptResponse) => {
    const done = pendingPrompts.get(id);
    pendingPrompts.delete(id);
    done?.(r);
  });
}
