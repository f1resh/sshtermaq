/**
 * Adds a key to Credentials, inline over the dialog that opened it (the session editor, Settings, the key generator):
 * - 'paste': a text area; the key is inspected while typing (type, fingerprint, passphrase protected) and stored
 *   encrypted under a name (prefilled from its comment);
 * - 'file': a key file chosen before; only its path is stored (a link), under a name (its comment, else the file name).
 */
import { useEffect, useRef, useState } from 'react';
import { KeyRound } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Modal, TextArea, TextInput } from '@/components/ui';
import { osTerms } from '@shared/platform';
import type { KeyInfo, VaultKey } from '@shared/types';
import { useVaultKeys, withUnlock } from '@/store/vault';
import { Callout, FormField, Spinner } from '../common/controls';
import { errorMessage } from '../common/hooks';
import { KeyCard } from './KeyCard';
import { nameTaken, suggestedKeyName } from './vaultModel';

export function AddKeyDialog({
  mode,
  path = '',
  initialText = '',
  onClose,
  onAdded
}: {
  mode: 'paste' | 'file';
  /** mode 'file': the chosen key file. */
  path?: string;
  /** mode 'paste': text to start with (the key generator's new key). */
  initialText?: string;
  onClose: () => void;
  onAdded: (key: VaultKey) => void;
}) {
  const keys = useVaultKeys();
  const [text, setText] = useState(initialText);
  const [info, setInfo] = useState<KeyInfo | null>(null);
  const [inspectError, setInspectError] = useState('');
  const [inspecting, setInspecting] = useState(false);
  const [name, setName] = useState('');
  const nameEdited = useRef(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  // Inspect the pasted text (debounced) or the file, and offer a name until the user types one.
  useEffect(() => {
    const source = mode === 'file' ? path : text;
    if (!source.trim()) {
      setInfo(null);
      setInspectError('');
      return;
    }
    let alive = true;
    setInspecting(true);
    const timer = setTimeout(
      () => {
        (mode === 'file' ? api.keys.inspect(path) : api.vault.inspectText(text))
          .then((i) => {
            if (!alive) return;
            setInfo(i);
            setInspectError('');
            if (!nameEdited.current) setName(suggestedKeyName(i, mode === 'file' ? path : ''));
          })
          .catch((e) => {
            if (!alive) return;
            setInfo(null);
            setInspectError(errorMessage(e));
          })
          .finally(() => alive && setInspecting(false));
      },
      mode === 'file' ? 0 : 250
    );
    return () => {
      alive = false;
      clearTimeout(timer);
    };
  }, [mode, path, text]);

  const taken = nameTaken(name, keys);
  const canAdd = !!info && !!name.trim() && !taken && !saving;

  const add = async () => {
    setSaving(true);
    setError('');
    try {
      const k = mode === 'file' ? await api.vault.addKeyFile(name.trim(), path) : await withUnlock(() => api.vault.addKeyText(name.trim(), text));
      if (k) onAdded(k);
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setSaving(false);
    }
  };

  return (
    <Modal
      open
      width={600}
      icon={<KeyRound size={18} />}
      title={mode === 'file' ? 'Add a key file to Credentials' : 'Paste a private key'}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" disabled={!canAdd} onClick={() => void add()}>
            {saving ? 'Adding…' : 'Add to Credentials'}
          </Button>
        </>
      }
    >
      <div className="vault-add">
        {mode === 'paste' ? (
          <FormField label="Private key" hint="OpenSSH, PEM or PuTTY .ppk: the whole text, including the BEGIN / END lines.">
            <TextArea
              value={text}
              onChange={setText}
              rows={8}
              className="mono-text vault-add__text"
              placeholder={'-----BEGIN OPENSSH PRIVATE KEY-----\n…\n-----END OPENSSH PRIVATE KEY-----'}
              aria-label="Private key text"
              spellCheck={false}
              autoFocus
            />
          </FormField>
        ) : (
          <FormField label="Key file">
            <div className="mono-text vault-add__path">{path}</div>
          </FormField>
        )}

        {inspecting && !info && <Spinner size={14} label="Reading the key…" />}
        {inspectError && <Callout tone="danger" title="Not a usable private key">{inspectError}</Callout>}
        {info && <KeyCard info={info} />}

        <FormField label="Name" hint="How the key is listed in Credentials and in a session's key choice." error={taken ? 'A key with this name already exists' : undefined}>
          <TextInput
            value={name}
            onChange={(v) => {
              nameEdited.current = true;
              setName(v);
            }}
            placeholder={suggestedKeyName(info, mode === 'file' ? path : '')}
            aria-label="Key name"
          />
        </FormField>

        <p className="muted vault-add__note">
          {mode === 'file'
            ? 'Only the path is stored: the file stays where it is, and SSHTermaq reads it at each connection.'
            : `The key is stored encrypted with ${osTerms.encryptedWith}, exactly as pasted (a passphrase stays on it).`}
        </p>
        {error && <Callout tone="danger" title="Could not add the key">{error}</Callout>}
      </div>
    </Modal>
  );
}
