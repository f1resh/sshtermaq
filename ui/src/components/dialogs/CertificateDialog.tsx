/**
 * A TLS certificate this computer does not trust (global prompt kind 'certificate', S3 endpoints such as a MinIO with
 * a self-signed certificate). Like an SSH host key:
 *  - unknown : Accept & Save (remembered in Known hosts) / Connect Once / Cancel
 *  - mismatch: another certificate than the accepted one -> Cancel (default) / Replace & connect (after confirming) /
 *    Connect once
 * Nothing signed has been sent to the server before the answer. Responds with choice 'accept-save' | 'accept-once' |
 * 'reject'.
 */
import { useState } from 'react';
import { ShieldAlert, ShieldQuestion } from 'lucide-react';
import { Button, Checkbox, Modal } from '@/components/ui';
import type { PromptRequest } from '@shared/types';
import { Callout, CopyButton } from './common/controls';
import { useInitialFocus, useStableCallback } from './common/hooks';
import type { HostKeyChoice } from './HostKeyDialog';

export type CertificatePrompt = Extract<PromptRequest, { kind: 'certificate' }>;

/** "AB:CD:…" groups of two, as browsers show certificate fingerprints. */
export function formatSha256(hex: string): string {
  return (hex.toUpperCase().match(/.{1,2}/g) ?? []).join(':');
}

function hostLabel(host: string, port: number): string {
  const h = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
  return port && port !== 443 ? `${h}:${port}` : h;
}

export function CertificateDialog({ prompt, onChoice }: { prompt: CertificatePrompt; onChoice: (c: HostKeyChoice) => void }) {
  const mismatch = prompt.status === 'mismatch';
  const [confirmed, setConfirmed] = useState(false);
  const reject = useStableCallback(() => onChoice('reject'));
  const cancelRef = useInitialFocus<HTMLButtonElement>(mismatch);
  const target = hostLabel(prompt.host, prompt.port);
  const fp = formatSha256(prompt.sha256);

  const details = (
    <div className="hostkey__key">
      <div className="hostkey__meta">
        <span>
          <span className="muted">Server</span> <strong className="mono-text">{target}</strong>
        </span>
        {prompt.subject && (
          <span>
            <span className="muted">Issued to</span> <strong>{prompt.subject}</strong>
          </span>
        )}
        {prompt.issuer && (
          <span>
            <span className="muted">Issued by</span> <strong>{prompt.issuer === prompt.subject ? `${prompt.issuer} (itself)` : prompt.issuer}</strong>
          </span>
        )}
        {(prompt.validFrom || prompt.validTo) && (
          <span>
            <span className="muted">Valid</span> {prompt.validFrom} – {prompt.validTo}
          </span>
        )}
      </div>
      <div className="hostkey__fp">
        <span className="hostkey__fp-label">SHA-256</span>
        <code className="hostkey__fp-value">{fp}</code>
        <CopyButton text={fp} label="Copy the SHA-256 fingerprint" />
      </div>
    </div>
  );

  if (!mismatch) {
    return (
      <Modal
        open
        dismissable={false}
        title="Certificate not trusted"
        icon={<ShieldQuestion size={20} className="dlg-title-icon dlg-title-icon--warning" />}
        onClose={reject}
        width={600}
        footer={
          <>
            <Button onClick={reject}>Cancel</Button>
            <Button onClick={() => onChoice('accept-once')}>Connect Once</Button>
            <Button variant="primary" onClick={() => onChoice('accept-save')}>
              Accept &amp; Save
            </Button>
          </>
        }
      >
        <div className="hostkey">
          <p>
            This computer does not trust the certificate of <strong className="mono-text">{target}</strong>
            {prompt.problem ? <>: {prompt.problem}</> : null}. Your own server (a MinIO with a self-signed certificate, for example) is
            often like this; anyone else&apos;s should not be.
          </p>
          {details}
          <p className="hostkey__help">
            Compare the fingerprint with the server&apos;s own (<code>openssl x509 -noout -fingerprint -sha256 -in public.crt</code>).
            <strong> Accept &amp; Save</strong> remembers this certificate in Known hosts; <strong>Connect Once</strong> trusts it until
            the session ends. Nothing has been sent to the server yet.
          </p>
        </div>
      </Modal>
    );
  }

  return (
    <Modal
      open
      dismissable={false}
      className="hostkey-modal--danger"
      title="Certificate changed: potential security breach"
      icon={<ShieldAlert size={20} className="dlg-title-icon dlg-title-icon--danger" />}
      onClose={reject}
      width={620}
      footer={
        <>
          <Button variant="ghost" className="dlg-footer__left" onClick={() => onChoice('accept-once')}>
            Connect once
          </Button>
          <Button variant="danger" disabled={!confirmed} onClick={() => onChoice('accept-save')}>
            Replace &amp; connect
          </Button>
          <Button ref={cancelRef} variant="primary" onClick={reject}>
            Cancel
          </Button>
        </>
      }
    >
      <div className="hostkey">
        <Callout tone="danger" title="The server presents another certificate than the one you accepted">
          Either its certificate was renewed, or another computer is pretending to be the server (a man-in-the-middle attack) and
          could read the files you transfer.
        </Callout>
        <div className="hostkey__subhead">Certificate presented now</div>
        {details}
        {prompt.previous.length > 0 && (
          <>
            <div className="hostkey__subhead">Accepted before</div>
            <ul className="hostkey__previous">
              {prompt.previous.map((p) => (
                <li key={p}>
                  <code>{formatSha256(p)}</code>
                </li>
              ))}
            </ul>
          </>
        )}
        <Checkbox
          className="hostkey__confirm"
          checked={confirmed}
          onChange={setConfirmed}
          label="I have checked the new fingerprint and want to replace the accepted certificate"
        />
      </div>
    </Modal>
  );
}
