/**
 * Settings › Credentials › Export and import: one file, encrypted with a password chosen for it, holding every key
 * (a stored key with its text, a linked key as its path) and login, and optionally the stored passwords and remembered
 * passphrases. The backup of Credentials, and the way to another computer (they only decrypt on this one otherwise).
 * Importing merges: what is already here is left as it is; ids are kept, so sessions restored from a session backup
 * find their keys and logins.
 */
import { useState } from 'react';
import { Download, FileUp, Upload } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Checkbox, Modal, toast } from '@/components/ui';
import { requestUnlock, useVault } from '@/store/vault';
import type { VaultImportResult } from '@shared/types';
import { Callout } from '../common/controls';
import { errorMessage } from '../common/hooks';
import { MasterDialog, MIN_MASTER, PasswordField, useNewPassword } from './MasterPassword';
import { importSummary, keysAndLogins } from './vaultModel';

type Open = 'export' | 'import' | null;

/** Unlocks first when a master password locks the secrets (the file dialogs must not run twice). */
async function unlockedFirst(): Promise<boolean> {
  return useVault.getState().status !== 'locked' || requestUnlock();
}

export function ExportImportSection() {
  const [open, setOpen] = useState<Open>(null);
  return (
    <>
      <h3 className="vault-section">Export and import</h3>
      <p className="muted settings-hint">
        Stored keys and passwords decrypt only on this computer. An exported file, protected by a password you choose, is their backup and the way to
        another computer.
      </p>
      <div className="kb-toolbar">
        <Button icon={<Download size={14} />} onClick={() => setOpen('export')}>
          Export Credentials…
        </Button>
        <Button icon={<Upload size={14} />} onClick={() => setOpen('import')}>
          Import Credentials…
        </Button>
      </div>
      {open === 'export' && <ExportDialog onClose={() => setOpen(null)} />}
      {open === 'import' && <ImportDialog onClose={() => setOpen(null)} />}
    </>
  );
}

function ExportDialog({ onClose }: { onClose: () => void }) {
  const n = useNewPassword();
  const [secrets, setSecrets] = useState(true);
  return (
    <MasterDialog
      title="Export Credentials"
      icon={<Download size={18} />}
      action="Export…"
      canSubmit={n.ok}
      onClose={onClose}
      submit={async () => {
        if (!(await unlockedFirst())) return 'Credentials are locked: unlock them to export.';
        const r = await api.vault.exportFile(n.pw, secrets);
        if (!r) return 'Not exported: no file was chosen.';
        toast.success('Credentials exported', `${keysAndLogins(r.keys, r.logins) || 'Nothing'} → ${r.path}`);
        if (r.skipped.length) toast.info('Some keys were left out', `${r.skipped.join(', ')}: their text cannot be read on this computer.`);
      }}
    >
      <PasswordField label="File password" value={n.pw} onChange={n.setPw} autoFocus hint={`Asked when the file is imported. At least ${MIN_MASTER} characters.`} error={n.short ? `Use at least ${MIN_MASTER} characters` : undefined} />
      <PasswordField label="Repeat it" value={n.again} onChange={n.setAgain} error={n.differs ? 'The two passwords differ' : undefined} />
      <Checkbox checked={secrets} onChange={setSecrets} label="Include stored passwords and remembered passphrases" />
      <p className="muted">
        Stored keys are always included (their text is the key). Linked key files are not copied: only their paths. Anyone with the file and its password
        can use what it holds: keep both safe.
      </p>
    </MasterDialog>
  );
}

function ImportDialog({ onClose }: { onClose: () => void }) {
  const [path, setPath] = useState('');
  const [pw, setPw] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [done, setDone] = useState<VaultImportResult | null>(null);
  const choose = async () => {
    const p = await api.dialog.openFile({ title: 'Import Credentials', filters: [{ name: 'Credentials', extensions: ['sshtcred'] }] });
    if (p) {
      setPath(p);
      setError('');
    }
  };
  const run = async () => {
    if (!path || !pw || busy) return;
    setBusy(true);
    setError('');
    try {
      if (!(await unlockedFirst())) return setError('Credentials are locked: unlock them to import.');
      const r = await api.vault.importFile(pw, path);
      if (r) setDone(r);
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      open
      width={500}
      icon={<Upload size={18} />}
      title="Import Credentials"
      onClose={onClose}
      footer={
        done ? (
          <Button variant="primary" onClick={onClose}>
            Close
          </Button>
        ) : (
          <>
            <Button onClick={onClose}>Cancel</Button>
            <Button variant="primary" disabled={!path || !pw || busy} onClick={() => void run()}>
              {busy ? 'Importing…' : 'Import'}
            </Button>
          </>
        )
      }
    >
      {done ? (
        <Callout tone="success" title="Imported">
          {importSummary(done).map((line) => (
            <div key={line}>{line}</div>
          ))}
        </Callout>
      ) : (
        <form
          className="vault-add"
          onSubmit={(e) => {
            e.preventDefault();
            void run();
          }}
        >
          <div className="vault-import-file">
            <Button icon={<FileUp size={14} />} onClick={() => void choose()}>
              Choose a file…
            </Button>
            <span className="mono-text vault-path" title={path}>
              {path || 'No file chosen'}
            </span>
          </div>
          <PasswordField label="File password" value={pw} onChange={setPw} />
          <p className="muted">Keys and logins already here are left as they are; a name that is taken gets &ldquo;(2)&rdquo;.</p>
          {error && <Callout tone="danger">{error}</Callout>}
          <button type="submit" hidden />
        </form>
      )}
    </Modal>
  );
}
