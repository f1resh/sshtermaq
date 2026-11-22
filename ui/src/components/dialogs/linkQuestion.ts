/**
 * The question before a clicked link (ssh://, sftp://, scp://, telnet://, rlogin://, s3://) connects: what it opens, and
 * the command it would run on this computer first (a local proxy command from Default Settings).
 */
import type { OpenRequest } from '@shared/types';

export interface LinkQuestion {
  title: string;
  message: string;
  /** The local proxy command, shown as it is configured ('' => none). */
  command: string;
}

const PROTOCOL_NAMES: Record<string, string> = { ssh: 'SSH', telnet: 'Telnet', rlogin: 'Rlogin', raw: 'Raw' };

function target(req: OpenRequest): string {
  const s = req.session;
  const host = s.host.includes(':') ? `[${s.host}]` : s.host;
  return `${s.username ? `${s.username}@` : ''}${host}:${s.port}`;
}

export function linkQuestion(req: OpenRequest): LinkQuestion {
  const s = req.session;
  if (s.protocol === 's3') {
    // s3:// runs no command; it signs in with the default AWS profile or the environment's keys.
    const where = `s3://${s.s3.bucket}${s.s3.startPath ? '/' + s.s3.startPath.replace(/^\/+/, '') : ''}`;
    const how = s.s3.auth.method === 'env' ? 'the keys in AWS_ACCESS_KEY_ID' : `the AWS profile "${s.s3.auth.profile || 'default'}"`;
    return { title: 'Open a link?', message: `Open the S3 storage ${where} with ${how}?`, command: '' };
  }
  const files = req.view === 'files';
  const name = files ? (s.ssh.fileProtocol === 'scp' ? 'SCP' : 'SFTP') : (PROTOCOL_NAMES[s.protocol] ?? s.protocol);
  const article = name === 'Telnet' || name === 'Raw' ? 'a' : 'an';  // "an SSH", "an SFTP", "an Rlogin", "a Telnet"
  const command = s.proxy.type === 'command' ? s.proxy.localCommand.trim() : '';
  const lines = [`Open ${article} ${name} connection to ${target(req)}${files && req.remotePath ? `, folder ${req.remotePath}` : ''}?`];
  if (command) lines.push('', 'It first runs this command on this computer (the local proxy command of Default Settings):');
  return { title: 'Open a link?', message: lines.join('\n'), command };
}
