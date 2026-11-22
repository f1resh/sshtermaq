import { describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import type { OpenRequest } from '@shared/types';
import { linkQuestion } from './linkQuestion';

function req(patch: Partial<OpenRequest['session']> = {}, extra: Partial<OpenRequest> = {}): OpenRequest {
  return { session: { ...createDefaultSession({ host: 'host.example', port: 22 }), ...patch }, fromLink: true, ...extra };
}

describe('linkQuestion', () => {
  it('says what the link opens', () => {
    expect(linkQuestion(req({ username: 'deploy' }))).toEqual({
      title: 'Open a link?',
      message: 'Open an SSH connection to deploy@host.example:22?',
      command: ''
    });
    expect(linkQuestion(req({ protocol: 'telnet', port: 23 })).message).toBe('Open a Telnet connection to host.example:23?');
    expect(linkQuestion(req({ host: 'fe80::1', port: 2222 })).message).toBe('Open an SSH connection to [fe80::1]:2222?');
  });

  it('names the file protocol and folder of sftp:// and scp:// links', () => {
    expect(linkQuestion(req({}, { view: 'files', remotePath: '/var/www' })).message).toBe(
      'Open an SFTP connection to host.example:22, folder /var/www?'
    );
    const s = createDefaultSession();
    expect(linkQuestion(req({ ssh: { ...s.ssh, fileProtocol: 'scp' } }, { view: 'files' })).message).toBe(
      'Open an SCP connection to host.example:22?'
    );
  });

  it('shows the local proxy command it would run first', () => {
    const s = createDefaultSession();
    const q = linkQuestion(req({ proxy: { ...s.proxy, type: 'command', localCommand: '  nc -X connect %host %port ' } }));
    expect(q.command).toBe('nc -X connect %host %port');
    expect(q.message).toBe(
      'Open an SSH connection to host.example:22?\n\nIt first runs this command on this computer (the local proxy command of Default Settings):'
    );
    // Another proxy type runs nothing here.
    expect(linkQuestion(req({ proxy: { ...s.proxy, type: 'socks5', localCommand: 'nc %host' } })).command).toBe('');
  });

  it('describes s3:// links (no command runs)', () => {
    const s = createDefaultSession();
    const q = linkQuestion(
      req({ protocol: 's3', host: '', port: 0, s3: { ...s.s3, bucket: 'logs', startPath: 'web/2026', auth: { ...s.s3.auth, method: 'profile' } } })
    );
    expect(q).toEqual({ title: 'Open a link?', message: 'Open the S3 storage s3://logs/web/2026 with the AWS profile "default"?', command: '' });
    const e = linkQuestion(req({ protocol: 's3', host: '', s3: { ...s.s3, bucket: 'b', auth: { ...s.s3.auth, method: 'env' } } }));
    expect(e.message).toBe('Open the S3 storage s3://b with the keys in AWS_ACCESS_KEY_ID?');
  });
});
