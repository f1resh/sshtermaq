import { describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import type { ImportCandidate, ImportPreview, ImportSource, SessionConfig } from '@shared/types';
import { commandsSummary, initialSelection, localCommands } from './importModel';

const defaults = createDefaultSession();
const proxied = (cmd: string): Partial<SessionConfig> => ({ proxy: { ...defaults.proxy, type: 'command', localCommand: cmd } });
const terminal = (cmd: string): Partial<SessionConfig> => ({ protocol: 'local', local: { ...defaults.local, command: cmd } });

function cand(key: string, s: Partial<SessionConfig> = {}, source: ImportSource = 'json'): ImportCandidate {
  return { key, name: key, source, session: createDefaultSession({ name: key, host: `${key}.example`, ...s }), warnings: [], exists: false };
}
function preview(source: ImportSource, candidates: ImportCandidate[]): ImportPreview {
  return { source, candidates, hostKeys: [], errors: [], origin: 'x' };
}

describe('commands an imported session runs on this computer', () => {
  it('finds the local proxy command and a local terminal command, nothing else', () => {
    expect(localCommands(cand('a', proxied('  nc %host %port ')).session)).toEqual([{ kind: 'proxy', command: 'nc %host %port' }]);
    expect(localCommands(cand('b', terminal('powershell -File x.ps1')).session)).toEqual([
      { kind: 'terminal', command: 'powershell -File x.ps1' }
    ]);
    expect(localCommands(cand('c').session)).toEqual([]);
    // The default shell (no command), an empty proxy command, a command kept for another proxy type: nothing runs.
    expect(localCommands(cand('d', terminal('')).session)).toEqual([]);
    expect(localCommands(cand('e', proxied('   ')).session)).toEqual([]);
    expect(localCommands(cand('f', { proxy: { ...defaults.proxy, type: 'socks5', localCommand: 'nc %host' } }).session)).toEqual([]);
    // A remote command runs on the server, not here.
    expect(localCommands(cand('g', { ssh: { ...defaults.ssh, remoteCommand: 'top' } }).session)).toEqual([]);
  });

  it('rows with a command from a file start unticked; from this computer they stay ticked', () => {
    const list = (source: ImportSource) => [cand('plain', {}, source), cand('proxy', proxied('nc %host %port'), source), cand('term', terminal('cmd /c x'), source)];
    expect([...initialSelection(preview('json', list('json')))]).toEqual(['plain']);
    expect([...initialSelection(preview('putty-regfile', list('putty-regfile')))]).toEqual(['plain']);
    expect([...initialSelection(preview('putty-registry', list('putty-registry')))]).toEqual(['plain', 'proxy', 'term']);
    expect([...initialSelection(preview('ssh-config', list('ssh-config')))]).toEqual(['plain', 'proxy', 'term']);
  });

  it('says how many of the selected ones run a command', () => {
    const list = [cand('plain'), cand('proxy', proxied('nc')), cand('term', terminal('x')), cand('other')];
    expect(commandsSummary(list, new Set(['plain', 'other']))).toBe('');
    expect(commandsSummary(list, new Set(['plain', 'proxy', 'other']))).toBe('1 of the 3 selected sessions runs a command on this computer');
    expect(commandsSummary(list, new Set(['plain', 'proxy', 'term', 'other']))).toBe('2 of the 4 selected sessions run a command on this computer');
    expect(commandsSummary(list, new Set(['term']))).toBe('The selected session runs a command on this computer');
    expect(commandsSummary(list, new Set())).toBe('');
  });
});
