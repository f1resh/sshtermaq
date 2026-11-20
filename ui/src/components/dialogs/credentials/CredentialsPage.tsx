/**
 * Settings › Credentials: keys and logins that sessions can use instead of settings of their own.
 * Keys: add (paste, link a file, generate); copy the public key, rename, show the file / export, delete.
 * Logins: a user name plus a password (stored / asked / none), a key and / or an agent; new, edit, duplicate, delete.
 * Deleting anything in use names the sessions (and logins) that use it.
 */
import { useState } from 'react';
import { ClipboardPaste, Copy, CopyPlus, Download, Eraser, FileKey2, FolderSearch, KeyRound, Pencil, Plus, Sparkles, Trash2, UserRound } from 'lucide-react';
import { api } from '@/bridge/api';
import { runCommand } from '@/commands/commands';
import { Button, IconButton, Modal, TextInput, toast } from '@/components/ui';
import { useSessions } from '@/store/data';
import { requestUnlock, useVaultKeys, useVaultLogins, useVaultStatus } from '@/store/vault';
import { osTerms } from '@shared/platform';
import type { VaultKey, VaultLogin } from '@shared/types';
import { Badge, Callout } from '../common/controls';
import { errorMessage } from '../common/hooks';
import { AddKeyDialog } from './AddKeyDialog';
import { ExportImportSection } from './ExportImport';
import { LoginDialog } from './LoginDialog';
import { MasterPasswordSection } from './MasterPassword';
import { loginSummary, loginsUsingKey, nameTaken, sessionsUsingKey, sessionsUsingLogin, shortKeyType } from './vaultModel';

type Adding = { mode: 'paste' } | { mode: 'file'; path: string } | null;
type LoginEdit = { login?: VaultLogin; duplicate?: boolean } | null;

const usedBy = (sessions: number, logins: number) =>
  [sessions ? `${sessions} session${sessions === 1 ? '' : 's'}` : '', logins ? `${logins} login${logins === 1 ? '' : 's'}` : '']
    .filter(Boolean)
    .join(' · ') || 'unused';

export function CredentialsPage() {
  const keys = useVaultKeys();
  const logins = useVaultLogins();
  const status = useVaultStatus();
  const sessions = useSessions((s) => s.sessions);
  const [loginEdit, setLoginEdit] = useState<LoginEdit>(null);
  const [deletingLogin, setDeletingLogin] = useState<VaultLogin | null>(null);
  const [adding, setAdding] = useState<Adding>(null);
  const [renaming, setRenaming] = useState<VaultKey | null>(null);
  const [deleting, setDeleting] = useState<VaultKey | null>(null);

  const linkFile = async () => {
    const path = await api.dialog.openFile({ title: 'Choose a private key file' });
    if (path) setAdding({ mode: 'file', path });
  };
  const copyPublic = (k: VaultKey) => {
    if (!k.publicKeyOpenSsh) return toast.info('No public key known', 'Its file needs the passphrase to show it.');
    api.app.writeClipboard(k.publicKeyOpenSsh);
    toast.success('Public key copied', k.name);
  };
  const exportKey = (k: VaultKey) =>
    void api.vault
      .exportKey(k.id)
      .then((p) => p && toast.success('Key exported', p))
      .catch((e) => toast.error('Export failed', errorMessage(e)));
  const forgetPassphrase = (k: VaultKey) =>
    void api.vault
      .forgetPassphrase(k.id)
      .then(() => toast.success('Passphrase forgotten', `${k.name}: it is asked at the next connection.`))
      .catch((e) => toast.error('Could not forget the passphrase', errorMessage(e)));

  return (
    <>
      <h2 className="dlg-page-title">Credentials</h2>
      <p className="muted settings-hint">
        Keys and logins that sessions can use instead of settings of their own. Pasted keys, stored passwords and remembered passphrases are encrypted
        with {osTerms.encryptedWith}, on this computer only: export a key to keep a copy. A linked key file stays where it is; only its path is kept. A
        key&apos;s passphrase is remembered only when you tick &ldquo;Remember for this key&rdquo; as you connect.
      </p>
      {status === 'locked' && (
        <Callout tone="warning" title="Credentials are locked">
          Stored keys, passwords and passphrases are encrypted with the master password. The first connection that needs one asks for it.{' '}
          <Button size="sm" onClick={() => void requestUnlock()}>
            Unlock…
          </Button>
        </Callout>
      )}
      <h3 className="vault-section">Keys</h3>
      <div className="kb-toolbar">
        <Button icon={<ClipboardPaste size={14} />} onClick={() => setAdding({ mode: 'paste' })}>
          Paste a key…
        </Button>
        <Button icon={<FileKey2 size={14} />} onClick={() => void linkFile()}>
          Link a key file…
        </Button>
        <Button icon={<Sparkles size={14} />} onClick={() => runCommand('app.keygen')}>
          Generate…
        </Button>
      </div>

      <div className="dlg-table vault-table">
        <div className="dlg-table__head vault-row">
          <span>Name</span>
          <span>Type</span>
          <span>Fingerprint</span>
          <span>Where</span>
          <span>Used by</span>
          <span />
        </div>
        <div className="dlg-table__body">
          {keys.map((k) => {
            const users = sessionsUsingKey(k.id, sessions).length;
            const viaLogins = loginsUsingKey(k.id, logins).length;
            return (
              <div key={k.id} className="dlg-table__row vault-row" data-key-id={k.id}>
                <span className="vault-name" title={k.comment || k.name}>
                  <KeyRound size={13} className="muted" /> {k.name}
                </span>
                <span className="muted">{shortKeyType(k.keyType)}</span>
                <span className="mono-text vault-fp" title={k.fingerprint}>
                  {k.fingerprint.replace('SHA256:', '') || '—'}
                </span>
                <span className="vault-where" title={k.kind === 'file' ? k.path : undefined}>
                  {k.kind === 'stored' ? <Badge>Stored</Badge> : <span className="mono-text vault-path">{k.path.split(/[\\/]/).pop()}</span>}
                  {k.missing && <Badge tone="danger">File missing</Badge>}
                  {(k.encrypted || k.passphraseRemembered) && (
                    <Badge tone="success">{k.passphraseRemembered ? 'Passphrase remembered' : 'Passphrase'}</Badge>
                  )}
                </span>
                <span className="muted">{usedBy(users, viaLogins)}</span>
                <span className="vault-actions">
                  <IconButton size="sm" label="Copy public key" onClick={() => copyPublic(k)}>
                    <Copy size={13} />
                  </IconButton>
                  <IconButton size="sm" label="Rename" onClick={() => setRenaming(k)}>
                    <Pencil size={13} />
                  </IconButton>
                  {k.kind === 'file' ? (
                    <IconButton size="sm" label={`Show in ${osTerms.fileBrowser}`} disabled={k.missing} onClick={() => void api.local.reveal(k.path).catch(() => undefined)}>
                      <FolderSearch size={13} />
                    </IconButton>
                  ) : (
                    <IconButton size="sm" label="Export to a file" onClick={() => exportKey(k)}>
                      <Download size={13} />
                    </IconButton>
                  )}
                  {k.passphraseRemembered && (
                    <IconButton size="sm" label="Forget passphrase" onClick={() => forgetPassphrase(k)}>
                      <Eraser size={13} />
                    </IconButton>
                  )}
                  <IconButton size="sm" label="Delete" onClick={() => setDeleting(k)}>
                    <Trash2 size={13} />
                  </IconButton>
                </span>
              </div>
            );
          })}
          {keys.length === 0 && <div className="dlg-table__empty">No keys yet. Paste one, or link a key file.</div>}
        </div>
      </div>

      <h3 className="vault-section">Logins</h3>
      <div className="kb-toolbar">
        <Button icon={<Plus size={14} />} onClick={() => setLoginEdit({})}>
          New login…
        </Button>
      </div>
      <div className="dlg-table vault-table">
        <div className="dlg-table__head vault-login-row">
          <span>Name</span>
          <span>User</span>
          <span>Signs in with</span>
          <span>Used by</span>
          <span />
        </div>
        <div className="dlg-table__body">
          {logins.map((l) => (
            <div key={l.id} className="dlg-table__row vault-login-row" data-login-id={l.id}>
              <span className="vault-name">
                <UserRound size={13} className="muted" /> {l.name}
              </span>
              <span className="mono-text">{l.username}</span>
              <span className="muted">{loginSummary(l, keys)}</span>
              <span className="muted">{usedBy(sessionsUsingLogin(l.id, sessions).length, 0)}</span>
              <span className="vault-actions">
                <IconButton size="sm" label="Edit" onClick={() => setLoginEdit({ login: l })}>
                  <Pencil size={13} />
                </IconButton>
                <IconButton size="sm" label="Duplicate" onClick={() => setLoginEdit({ login: l, duplicate: true })}>
                  <CopyPlus size={13} />
                </IconButton>
                <IconButton size="sm" label="Delete" onClick={() => setDeletingLogin(l)}>
                  <Trash2 size={13} />
                </IconButton>
              </span>
            </div>
          ))}
          {logins.length === 0 && <div className="dlg-table__empty">No logins yet. A login is a user name with a password, a key and / or the agent.</div>}
        </div>
      </div>

      <ExportImportSection />
      <MasterPasswordSection />

      {adding && (
        <AddKeyDialog
          mode={adding.mode}
          path={adding.mode === 'file' ? adding.path : ''}
          onClose={() => setAdding(null)}
          onAdded={(k) => {
            setAdding(null);
            toast.success('Key added', k.name);
          }}
        />
      )}
      {renaming && <RenameKeyDialog keyItem={renaming} keys={keys} onClose={() => setRenaming(null)} />}
      {deleting && (
        <DeleteKeyDialog
          keyItem={deleting}
          sessionNames={sessionsUsingKey(deleting.id, sessions).map((s) => s.name)}
          loginNames={loginsUsingKey(deleting.id, logins).map((l) => l.name)}
          onClose={() => setDeleting(null)}
        />
      )}
      {loginEdit && (
        <LoginDialog
          login={loginEdit.login}
          duplicate={loginEdit.duplicate}
          onClose={() => setLoginEdit(null)}
          onSaved={(l) => {
            setLoginEdit(null);
            toast.success(loginEdit.login && !loginEdit.duplicate ? 'Login saved' : 'Login added', l.name);
          }}
        />
      )}
      {deletingLogin && (
        <DeleteLoginDialog login={deletingLogin} users={sessionsUsingLogin(deletingLogin.id, sessions).map((s) => s.name)} onClose={() => setDeletingLogin(null)} />
      )}
    </>
  );
}

function RenameKeyDialog({ keyItem, keys, onClose }: { keyItem: VaultKey; keys: VaultKey[]; onClose: () => void }) {
  const [name, setName] = useState(keyItem.name);
  const [error, setError] = useState('');
  const taken = nameTaken(name, keys, keyItem.id);
  const save = () =>
    void api.vault
      .rename(keyItem.id, name.trim())
      .then(onClose)
      .catch((e) => setError(errorMessage(e)));
  return (
    <Modal
      open
      width={420}
      title={`Rename "${keyItem.name}"`}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" disabled={!name.trim() || taken || name.trim() === keyItem.name} onClick={save}>
            Rename
          </Button>
        </>
      }
    >
      <TextInput value={name} onChange={setName} aria-label="Key name" autoFocus onKeyDown={(e) => e.key === 'Enter' && name.trim() && !taken && save()} />
      {taken && <p className="vault-add__error">A key with this name already exists.</p>}
      {error && <Callout tone="danger">{error}</Callout>}
    </Modal>
  );
}

function DeleteKeyDialog({
  keyItem,
  sessionNames,
  loginNames,
  onClose
}: {
  keyItem: VaultKey;
  sessionNames: string[];
  loginNames: string[];
  onClose: () => void;
}) {
  const users = [...sessionNames, ...loginNames.map((n) => `login ${n}`)];
  const title = `Used by ${usedBy(sessionNames.length, loginNames.length).replace(' · ', ' and ')}`;
  const remove = () =>
    void api.vault
      .delete(keyItem.id)
      .then(onClose)
      .catch((e) => toast.error('Could not delete the key', errorMessage(e)));
  return (
    <Modal
      open
      width={460}
      title={`Delete "${keyItem.name}"?`}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="danger" onClick={remove}>
            Delete
          </Button>
        </>
      }
    >
      <p>
        {keyItem.kind === 'stored'
          ? 'The stored key is deleted from this computer. Export it first if you need it again.'
          : 'Only the link is removed: the key file itself stays where it is.'}
        {keyItem.passphraseRemembered && ' Its remembered passphrase is forgotten too.'}
      </p>
      {users.length > 0 && (
        <Callout tone="warning" title={title}>
          {users.join(', ')}. They will have no key until you choose another one.
        </Callout>
      )}
    </Modal>
  );
}

function DeleteLoginDialog({ login, users, onClose }: { login: VaultLogin; users: string[]; onClose: () => void }) {
  const remove = () =>
    void api.vault
      .deleteLogin(login.id)
      .then(onClose)
      .catch((e) => toast.error('Could not delete the login', errorMessage(e)));
  return (
    <Modal
      open
      width={460}
      title={`Delete login "${login.name}"?`}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="danger" onClick={remove}>
            Delete
          </Button>
        </>
      }
    >
      <p>{login.password === 'stored' ? 'Its stored password is deleted too.' : 'The login is removed from Credentials.'}</p>
      {users.length > 0 && (
        <Callout tone="warning" title={`Used by ${users.length} session${users.length === 1 ? '' : 's'}`}>
          {users.join(', ')}. They will sign in with their own settings again.
        </Callout>
      )}
    </Modal>
  );
}
