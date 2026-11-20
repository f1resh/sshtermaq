/** The public details of a key (type, format, passphrase, fingerprint, comment), as the session editor shows a key file. */
import { KeyRound } from 'lucide-react';
import type { KeyInfo } from '@shared/types';
import { Badge } from '../common/controls';

export const FORMAT_LABELS: Record<KeyInfo['format'], string> = {
  openssh: 'OpenSSH',
  pem: 'PEM',
  ppk2: 'PuTTY v2',
  ppk3: 'PuTTY v3',
  unknown: 'Unknown'
};

export function KeyCard({ info, extra }: { info: Pick<KeyInfo, 'keyType' | 'format' | 'encrypted' | 'fingerprint' | 'comment'>; extra?: React.ReactNode }) {
  return (
    <div className="keyinfo">
      <div className="keyinfo__icon">
        <KeyRound size={18} />
      </div>
      <div className="keyinfo__body">
        <div className="keyinfo__head">
          <span className="mono-text">{info.keyType || 'unknown key type'}</span>
          <Badge>{FORMAT_LABELS[info.format]}</Badge>
          {info.encrypted ? <Badge tone="success">Passphrase protected</Badge> : <Badge tone="warning">No passphrase</Badge>}
          {extra}
        </div>
        {info.fingerprint && <div className="keyinfo__fp mono-text">{info.fingerprint}</div>}
        {info.comment && <div className="keyinfo__comment">{info.comment}</div>}
      </div>
    </div>
  );
}
