/**
 * Creates, edits or duplicates a login of Credentials (inline over the dialog that opened it): a name, a user name,
 * and how to sign in: a password (stored, asked every time, or none), a key from Credentials, an SSH agent (also how
 * hardware keys work). A stored password goes to the host when typed and never comes back; editing leaves it as it
 * is unless a new one is typed.
 */
import { useState } from 'react';
import { UserRound } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Modal, Select, TextInput } from '@/components/ui';
import { useVaultKeys, useVaultLogins, withUnlock } from '@/store/vault';
import { osTerms } from '@shared/platform';
import type { LoginPassword, VaultLogin } from '@shared/types';
import { Callout, FormField, SegmentedControl, ToggleRow, type SegmentOption } from '../common/controls';
import { errorMessage } from '../common/hooks';
import { keyLabel, loginNameTaken } from './vaultModel';

const PASSWORD_MODES: readonly SegmentOption<LoginPassword>[] = [
  { value: 'stored', label: 'Stored', title: 'Saved encrypted and sent when the server asks' },
  { value: 'ask', label: 'Ask every time', title: 'Asked at each connection, never saved' },
  { value: 'none', label: 'None', title: 'No password sign-in (a key or the agent only)' }
];

export function LoginDialog({
  login,
  duplicate = false,
  onClose,
  onSaved
}: {
  /** The login to edit (or to copy, with `duplicate`); none = a new login. */
  login?: VaultLogin;
  duplicate?: boolean;
  onClose: () => void;
  onSaved: (login: VaultLogin) => void;
}) {
  const keys = useVaultKeys();
  const logins = useVaultLogins();
  const editing = !!login && !duplicate;
  const [name, setName] = useState(login ? (duplicate ? `${login.name} (copy)` : login.name) : '');
  const [username, setUsername] = useState(login?.username ?? '');
  const [mode, setMode] = useState<LoginPassword>(login?.password ?? 'stored');
  const [password, setPassword] = useState('');
  const [keyId, setKeyId] = useState(login?.keyId ?? '');
  const [useAgent, setUseAgent] = useState(login?.useAgent ?? false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  // A stored password exists already only when editing a login that stores one.
  const hasStored = editing && login?.password === 'stored';
  const taken = loginNameTaken(name, logins, editing ? login?.id : '');
  const noMethod = mode === 'none' && !keyId && !useAgent;
  const needsPassword = mode === 'stored' && !hasStored && !password;
  const canSave = !!name.trim() && !!username.trim() && !taken && !noMethod && !needsPassword && !saving;

  const save = async () => {
    setSaving(true);
    setError('');
    try {
      const fields = { name: name.trim(), username: username.trim(), password: mode, keyId, useAgent };
      const pw = mode === 'stored' && password ? password : null;
      const saved = await withUnlock(() => (editing && login ? api.vault.updateLogin({ ...login, ...fields }, pw) : api.vault.addLogin(fields, pw)));
      onSaved(saved);
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setSaving(false);
    }
  };

  return (
    <Modal
      open
      width={560}
      icon={<UserRound size={18} />}
      title={editing ? `Edit login "${login?.name}"` : 'New login'}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" disabled={!canSave} onClick={() => void save()}>
            {saving ? 'Saving…' : editing ? 'Save' : 'Add login'}
          </Button>
        </>
      }
    >
      <div className="vault-add">
        <FormField label="Name" hint="How the login is listed, e.g. deploy@prod." error={taken ? 'A login with this name already exists' : undefined}>
          <TextInput value={name} onChange={setName} aria-label="Login name" autoFocus placeholder="deploy@prod" />
        </FormField>
        <FormField label="User name">
          <TextInput value={username} onChange={setUsername} aria-label="User name" placeholder="deploy" />
        </FormField>
        <FormField label="Password">
          <SegmentedControl value={mode} options={PASSWORD_MODES} onChange={setMode} size="sm" ariaLabel="Password" />
        </FormField>
        {mode === 'stored' && (
          <FormField
            label={hasStored ? 'New password' : 'Password to store'}
            hint={hasStored ? 'Leave empty to keep the stored password.' : `Encrypted with ${osTerms.encryptedWith}.`}
          >
            <TextInput type="password" value={password} onChange={setPassword} aria-label="Password to store" autoComplete="new-password" placeholder={hasStored ? '(unchanged)' : ''} />
          </FormField>
        )}
        <FormField label="Key" hint="A key from Credentials, tried before the password.">
          <Select
            value={keyId}
            aria-label="Login key"
            options={[{ value: '', label: '(none)' }, ...keys.map((k) => ({ value: k.id, label: keyLabel(k) })), ...(keyId && !keys.some((k) => k.id === keyId) ? [{ value: keyId, label: '(a key no longer in Credentials)' }] : [])]}
            onChange={setKeyId}
          />
        </FormField>
        <ToggleRow
          label="Try an SSH agent"
          description="Keys held by Pageant, the Windows OpenSSH agent or ssh-agent; also hardware keys (YubiKey, FIDO) kept there."
          checked={useAgent}
          onChange={setUseAgent}
        />
        {noMethod && <Callout tone="warning">Choose at least one way to sign in: a password, a key or the agent.</Callout>}
        {error && <Callout tone="danger" title="Could not save the login">{error}</Callout>}
      </div>
    </Modal>
  );
}
