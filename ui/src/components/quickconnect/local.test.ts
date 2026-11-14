import { describe, expect, it } from 'vitest';
import type { LocalShellInfo } from '@shared/types';
import { localPart, resolveLocalTarget, shellToOpen } from './local';

const sh = (id: string, name: string, isDefault = false): LocalShellInfo => ({ id, name, command: `${id}.exe`, environment: [], isDefault });
const shells = [sh('cmd', 'Command Prompt'), sh('pwsh', 'PowerShell 7', true), sh('git-bash', 'Git Bash'), sh('wsl:Ubuntu-24.04', 'Ubuntu-24.04 (WSL)')];

describe('quick connect to a local terminal', () => {
  it('recognises local: targets', () => {
    expect(localPart('local:pwsh')).toBe('pwsh');
    expect(localPart('  LOCAL:  git bash ')).toBe('git bash');
    expect(localPart('local:')).toBe('');
    expect(localPart('user@host')).toBeNull();
  });

  it('finds a shell by id or name, also by the start of its name', () => {
    expect(resolveLocalTarget('pwsh', shells, shells[1]).shell?.id).toBe('pwsh');
    expect(resolveLocalTarget('git bash', shells, shells[1]).shell?.id).toBe('git-bash');
    expect(resolveLocalTarget('ubuntu', shells, shells[1]).shell?.id).toBe('wsl:Ubuntu-24.04');
    expect(resolveLocalTarget('Command', shells, shells[1]).shell?.id).toBe('cmd');
  });

  it('empty is the default shell; anything else is a command line', () => {
    expect(resolveLocalTarget('', shells, shells[1]).shell?.id).toBe('pwsh');
    const custom = resolveLocalTarget('python -i', shells, shells[1]);
    expect(custom).toEqual({ shell: null, command: 'python -i' });
    expect(shellToOpen(custom)).toMatchObject({ command: 'python -i', environment: [] });
    expect(shellToOpen({ shell: null, command: '' })).toBeNull();
  });
});
