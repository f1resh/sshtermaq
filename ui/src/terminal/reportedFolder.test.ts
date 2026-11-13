import { describe, expect, it } from 'vitest';
import { reportedFolder } from './termUtil';

describe('the folder a local shell reports (OSC 7 / OSC 9;9)', () => {
  it('macOS: file:// URLs, decoded; nothing else', () => {
    expect(reportedFolder(7, 'file://mac.local/Users/me/My%20Projects', false)).toBe('/Users/me/My Projects');
    expect(reportedFolder(7, 'file:///tmp', false)).toBe('/tmp');
    expect(reportedFolder(7, 'not a url', false)).toBe('');
    expect(reportedFolder(9, String.raw`9;"C:\Users"`, false)).toBe('');
  });

  it('Windows: drive paths from every kind of shell', () => {
    expect(reportedFolder(9, String.raw`9;"C:\Users\me"`, true)).toBe(String.raw`C:\Users\me`); // PowerShell / cmd
    expect(reportedFolder(9, '4;1;50', true)).toBe(''); // other uses of OSC 9
    expect(reportedFolder(7, 'file://host/C:/Repos/SSHTermaq', true)).toBe(String.raw`C:\Repos\SSHTermaq`);
    expect(reportedFolder(7, 'file://host/d/work', true)).toBe(String.raw`D:\work`); // Git Bash / MSYS2
    expect(reportedFolder(7, 'file://host/cygdrive/c/Users', true)).toBe(String.raw`C:\Users`); // Cygwin
    expect(reportedFolder(7, 'file://host/c', true)).toBe('C:\\');
    expect(reportedFolder(7, 'file://ubuntu/home/me', true)).toBe(''); // WSL: not a Windows folder
    expect(reportedFolder(7, 'file://host/%E2%82%AC%ZZ', true)).toBe(''); // broken encoding
  });
});
