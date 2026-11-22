/**
 * "Copy link…" for a file on S3: a presigned URL anyone can open until it expires (1 hour, 1 day, 7 days or a time
 * the user gives, at most 7 days: S3's limit). A link made with temporary credentials (SSO, an assumed role) stops
 * working when they expire, usually within hours: the dialog says so.
 */
import { useState } from 'react';
import { Link2 } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Modal, Select, toast } from '@/components/ui';
import type { S3AuthMethod } from '@shared/types';

export const MAX_LINK_SECONDS = 7 * 24 * 3600;

export type LinkUnit = 'minutes' | 'hours' | 'days';
const UNIT_SECONDS: Record<LinkUnit, number> = { minutes: 60, hours: 3600, days: 86400 };

/** Seconds for "Other…": a whole number of units, 1 second .. 7 days; null when not valid. */
export function linkSeconds(amount: string, unit: LinkUnit): number | null {
  if (!/^\d{1,6}$/.test(amount.trim())) return null;
  const n = Number(amount) * UNIT_SECONDS[unit];
  return n >= 1 && n <= MAX_LINK_SECONDS ? n : null;
}

type Choice = '3600' | '86400' | '604800' | 'other';
const CHOICES: { value: Choice; label: string }[] = [
  { value: '3600', label: '1 hour' },
  { value: '86400', label: '1 day' },
  { value: '604800', label: '7 days' },
  { value: 'other', label: 'Other…' }
];

export function LinkDialog({
  sftpId,
  path,
  authMethod,
  onClose
}: {
  sftpId: string;
  path: string;
  authMethod: S3AuthMethod;
  onClose: () => void;
}) {
  const [choice, setChoice] = useState<Choice>('3600');
  const [amount, setAmount] = useState('12');
  const [unit, setUnit] = useState<LinkUnit>('hours');
  const [url, setUrl] = useState('');
  const [busy, setBusy] = useState(false);
  const seconds = choice === 'other' ? linkSeconds(amount, unit) : Number(choice);
  const name = path.slice(path.lastIndexOf('/') + 1);

  const create = async () => {
    if (!seconds) return;
    setBusy(true);
    try {
      const link = await api.sftp.presign(sftpId, path, seconds);
      setUrl(link);
      await api.app.writeClipboard(link);
      toast.success('Link copied', `${name}: anyone with the link can download it until it expires.`);
    } catch (e) {
      toast.error('No link', e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal
      open
      title={`Link to ${name}`}
      icon={<Link2 size={16} />}
      onClose={onClose}
      width={520}
      footer={
        <>
          <Button onClick={onClose}>{url ? 'Close' : 'Cancel'}</Button>
          <Button variant="primary" disabled={!seconds || busy} onClick={() => void create()}>
            {url ? 'Copy a new link' : 'Copy link'}
          </Button>
        </>
      }
    >
      <div className="fm-link">
        <label className="fm-link__row">
          <span>Valid for</span>
          <Select value={choice} options={CHOICES} onChange={(v) => setChoice(v as Choice)} aria-label="Valid for" />
          {choice === 'other' && (
            <>
              <input
                className="input fm-link__amount"
                value={amount}
                inputMode="numeric"
                aria-label="Amount"
                onChange={(e) => setAmount(e.target.value)}
                aria-invalid={!seconds || undefined}
              />
              <Select
                value={unit}
                options={[
                  { value: 'minutes', label: 'minutes' },
                  { value: 'hours', label: 'hours' },
                  { value: 'days', label: 'days' }
                ]}
                onChange={(v) => setUnit(v as LinkUnit)}
                aria-label="Unit"
              />
            </>
          )}
        </label>
        {choice === 'other' && !seconds && <p className="fm-link__error">Between 1 minute and 7 days (S3 allows no longer).</p>}
        <p className="muted">Anyone who has the link can download the file until it expires; it cannot be withdrawn earlier.</p>
        {authMethod !== 'keys' && authMethod !== 'anonymous' && (
          <p className="muted">
            Signed in with {authMethod === 'profile' ? 'a profile' : 'the environment'}: if these are temporary credentials (SSO, an
            assumed role, a session token), the link stops working when they expire, which can be sooner.
          </p>
        )}
        {url && <textarea className="input fm-link__url" readOnly value={url} rows={4} aria-label="Link" onFocus={(e) => e.currentTarget.select()} />}
      </div>
    </Modal>
  );
}
