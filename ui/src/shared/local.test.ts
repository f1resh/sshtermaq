import { describe, expect, it } from 'vitest';
import { createDefaultSession, createLocalSession, localProgramName, sessionLabel } from './defaults';

describe('local terminals', () => {
  it('names the program for labels', () => {
    expect(localProgramName(String.raw`"C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo`)).toBe('pwsh');
    expect(localProgramName(String.raw`C:\Windows\System32\cmd.exe`)).toBe('cmd');
    expect(localProgramName('/bin/zsh -l')).toBe('zsh');
    expect(localProgramName("'/opt/my shells/fish' -l")).toBe('fish');
    expect(localProgramName('  ')).toBe('Local terminal');
  });

  it('builds an unsaved session from a shell, keeping the template', () => {
    const template = createDefaultSession({ id: 'default', name: 'Default Settings' });
    template.terminal.scrollbackLines = 5000;
    const s = createLocalSession(
      { id: 'msys2', name: 'MSYS2', command: String.raw`"C:\msys64\usr\bin\bash.exe" --login -i`, environment: [{ name: 'MSYSTEM', value: 'UCRT64' }], isDefault: false },
      template
    );
    expect(s.protocol).toBe('local');
    expect(s.id).not.toBe('default');
    expect(s.name).toBe('');
    expect(s.local.command).toContain('bash.exe');
    expect(s.connection.environment).toEqual([{ name: 'MSYSTEM', value: 'UCRT64' }]);
    expect(s.terminal.closeOnExit).toBe('clean');
    expect(s.terminal.scrollbackLines).toBe(5000);
    expect(sessionLabel(s)).toBe('bash');
  });
});
