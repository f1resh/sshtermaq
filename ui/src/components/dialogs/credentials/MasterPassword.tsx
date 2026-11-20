/**
 * The optional master password of Credentials: the page's section (set, unlock, lock now, change, remove, reset) and
 * its dialogs, plus UnlockHost: the "Unlock Credentials" dialog any part of the UI can ask for (requestUnlock /
 * withUnlock in @/store/vault), mounted once at the app root.
 *
 * The secrets of Credentials (stored keys, stored passwords, remembered passphrases) are then sealed with it on top of
 * the OS protection; Credentials are locked after each start, and a connection that needs a secret asks for it once.
 */
import { useState, type ReactNode } from 'react';
import { Lock, LockOpen, ShieldCheck } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Modal, TextInput, toast } from '@/components/ui';
import { finishUnlock, useVault, useVaultStatus } from '@/store/vault';
import { osTerms } from '@shared/platform';
import { Badge, Callout, FormField } from '../common/controls';
import { errorMessage } from '../common/hooks';

export const MIN_MASTER = 8;

type Open = 'set' | 'unlock' | 'change' | 'remove' | 'reset' | null;

/** Settings › Credentials › Master password. */
export function MasterPasswordSection() {
  const status = useVaultStatus();
  const [open, setOpen] = useState<Open>(null);
  const close = () => setOpen(null);
  const lockNow = () =>
    void api.vault
      .lock()
      .then(() => toast.success('Credentials locked', 'The master password is asked again when a secret is needed.'))
      .catch((e) => toast.error('Could not lock', errorMessage(e)));

  return (
    <>
      <h3 className="vault-section">
        Master password{' '}
        {status === 'none' ? <Badge>Off</Badge> : status === 'locked' ? <Badge tone="warning">Locked</Badge> : <Badge tone="success">Unlocked</Badge>}
      </h3>
      {status === 'none' ? (
        <p className="muted settings-hint">
          Off: stored keys, passwords and passphrases are protected by {osTerms.encryptedWith} only. With a master password they are also encrypted with
          it, and asked for once after each start. If you forget it, they cannot be recovered: &ldquo;Reset Credentials&rdquo; then deletes them (linked
          key files, logins&apos; user names and your sessions stay).
        </p>
      ) : (
        <p className="muted settings-hint">
          {status === 'locked'
            ? 'Locked: the first connection that needs a stored key, password or passphrase asks for the master password; or unlock here.'
            : 'Unlocked until the app quits or you lock Credentials.'}
        </p>
      )}
      <div className="kb-toolbar vault-master">
        {status === 'none' && (
          <Button icon={<ShieldCheck size={14} />} onClick={() => setOpen('set')}>
            Set a master password…
          </Button>
        )}
        {status === 'locked' && (
          <>
            <Button icon={<LockOpen size={14} />} onClick={() => setOpen('unlock')}>
              Unlock…
            </Button>
            <Button onClick={() => setOpen('reset')}>Forgot it: reset Credentials…</Button>
          </>
        )}
        {status === 'unlocked' && (
          <>
            <Button icon={<Lock size={14} />} onClick={lockNow}>
              Lock now
            </Button>
            <Button onClick={() => setOpen('change')}>Change…</Button>
            <Button onClick={() => setOpen('remove')}>Remove…</Button>
          </>
        )}
      </div>
      {open === 'set' && <SetMasterDialog onClose={close} />}
      {open === 'unlock' && <UnlockDialog onDone={close} />}
      {open === 'change' && <ChangeMasterDialog onClose={close} />}
      {open === 'remove' && <RemoveMasterDialog onClose={close} />}
      {open === 'reset' && <ResetDialog onClose={close} />}
    </>
  );
}

/** A small form dialog: its fields, an error line, and a primary button that runs `submit`. */
export function MasterDialog({
  title,
  icon,
  action,
  danger,
  canSubmit,
  submit,
  onClose,
  children
}: {
  title: string;
  /** The title's icon (a lock unless given). */
  icon?: ReactNode;
  action: string;
  danger?: boolean;
  canSubmit: boolean;
  /** Resolves with an error message to show, or nothing when done. */
  submit: () => Promise<string | void>;
  onClose: () => void;
  children: ReactNode;
}) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const run = async () => {
    if (!canSubmit || busy) return;
    setBusy(true);
    setError('');
    try {
      const message = await submit();
      if (message) setError(message);
      else onClose();
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      open
      width={480}
      icon={icon ?? <Lock size={18} />}
      title={title}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant={danger ? 'danger' : 'primary'} disabled={!canSubmit || busy} onClick={() => void run()}>
            {busy ? 'Working…' : action}
          </Button>
        </>
      }
    >
      <form
        className="vault-add"
        onSubmit={(e) => {
          e.preventDefault();
          void run();
        }}
      >
        {children}
        {error && <Callout tone="danger">{error}</Callout>}
        <button type="submit" hidden />
      </form>
    </Modal>
  );
}

export function PasswordField({ label, value, onChange, autoFocus, hint, error }: { label: string; value: string; onChange: (v: string) => void; autoFocus?: boolean; hint?: string; error?: string }) {
  return (
    <FormField label={label} hint={hint} error={error}>
      <TextInput type="password" value={value} onChange={onChange} aria-label={label} autoFocus={autoFocus} autoComplete="new-password" />
    </FormField>
  );
}

/** A new password typed twice: long enough and the same. */
export function useNewPassword() {
  const [pw, setPw] = useState('');
  const [again, setAgain] = useState('');
  const short = pw.length > 0 && pw.length < MIN_MASTER;
  const differs = again.length > 0 && again !== pw;
  return { pw, setPw, again, setAgain, short, differs, ok: pw.length >= MIN_MASTER && again === pw };
}

function SetMasterDialog({ onClose }: { onClose: () => void }) {
  const n = useNewPassword();
  return (
    <MasterDialog
      title="Set a master password"
      action="Set master password"
      canSubmit={n.ok}
      onClose={onClose}
      submit={async () => {
        await api.vault.setMaster(n.pw);
        toast.success('Master password set', 'Stored keys, passwords and passphrases are now encrypted with it too.');
      }}
    >
      <PasswordField label="Master password" value={n.pw} onChange={n.setPw} autoFocus hint={`At least ${MIN_MASTER} characters.`} error={n.short ? `Use at least ${MIN_MASTER} characters` : undefined} />
      <PasswordField label="Repeat it" value={n.again} onChange={n.setAgain} error={n.differs ? 'The two passwords differ' : undefined} />
      <Callout tone="warning" title="It cannot be recovered">
        If you forget it, the stored keys, passwords and passphrases are lost: &ldquo;Reset Credentials&rdquo; deletes them. Linked key files, logins&apos;
        user names and your sessions stay. Export stored keys first if you have no other copy.
      </Callout>
    </MasterDialog>
  );
}

/** Unlock: from the page (onDone) or for UnlockHost. */
function UnlockDialog({ onDone }: { onDone: (unlocked: boolean) => void }) {
  const [pw, setPw] = useState('');
  return (
    <MasterDialog
      title="Unlock Credentials"
      action="Unlock"
      canSubmit={pw.length > 0}
      onClose={() => onDone(false)}
      submit={async () => {
        if (!(await api.vault.unlock(pw))) {
          setPw('');
          return 'Wrong master password, try again.';
        }
        onDone(true);
      }}
    >
      <p className="muted">Enter the master password of Credentials. It is asked once until the app quits or you lock them.</p>
      <PasswordField label="Master password" value={pw} onChange={setPw} autoFocus />
    </MasterDialog>
  );
}

function ChangeMasterDialog({ onClose }: { onClose: () => void }) {
  const [current, setCurrent] = useState('');
  const n = useNewPassword();
  return (
    <MasterDialog
      title="Change the master password"
      action="Change"
      canSubmit={current.length > 0 && n.ok}
      onClose={onClose}
      submit={async () => {
        await api.vault.changeMaster(current, n.pw);
        toast.success('Master password changed');
      }}
    >
      <PasswordField label="Current master password" value={current} onChange={setCurrent} autoFocus />
      <PasswordField label="New master password" value={n.pw} onChange={n.setPw} error={n.short ? `Use at least ${MIN_MASTER} characters` : undefined} />
      <PasswordField label="Repeat the new one" value={n.again} onChange={n.setAgain} error={n.differs ? 'The two passwords differ' : undefined} />
    </MasterDialog>
  );
}

function RemoveMasterDialog({ onClose }: { onClose: () => void }) {
  const [current, setCurrent] = useState('');
  return (
    <MasterDialog
      title="Remove the master password"
      action="Remove"
      canSubmit={current.length > 0}
      onClose={onClose}
      submit={async () => {
        await api.vault.removeMaster(current);
        toast.success('Master password removed', `Credentials are protected by ${osTerms.encryptedWith} only.`);
      }}
    >
      <p className="muted">Stored keys, passwords and passphrases stay, protected by {osTerms.encryptedWith} only.</p>
      <PasswordField label="Current master password" value={current} onChange={setCurrent} autoFocus />
    </MasterDialog>
  );
}

function ResetDialog({ onClose }: { onClose: () => void }) {
  const keys = useVault((s) => s.keys);
  const logins = useVault((s) => s.logins);
  const stored = keys.filter((k) => k.kind === 'stored').length;
  const passwords = logins.filter((l) => l.password === 'stored').length;
  const passphrases = keys.filter((k) => k.passphraseRemembered).length;
  return (
    <MasterDialog
      title="Reset Credentials?"
      action="Delete the stored secrets"
      danger
      canSubmit
      onClose={onClose}
      submit={async () => {
        await api.vault.reset();
        toast.success('Credentials reset', 'The master password and the stored secrets are gone.');
      }}
    >
      <p>For a forgotten master password. This deletes, and nothing can bring back:</p>
      <ul className="vault-reset-list">
        <li>
          {stored} stored key{stored === 1 ? '' : 's'} (sessions using them will have no key)
        </li>
        <li>
          {passwords} login password{passwords === 1 ? '' : 's'} (those logins then ask for the password)
        </li>
        <li>
          {passphrases} remembered passphrase{passphrases === 1 ? '' : 's'}
        </li>
      </ul>
      <p className="muted">Linked key files, logins, their user names and your sessions stay. The master password is removed.</p>
    </MasterDialog>
  );
}

/** The app-wide "Unlock Credentials" dialog for requestUnlock(); mount once. */
export function UnlockHost() {
  const pending = useVault((s) => s.unlockRequest);
  if (!pending) return null;
  return <UnlockDialog onDone={finishUnlock} />;
}
