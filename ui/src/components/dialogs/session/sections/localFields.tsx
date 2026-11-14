/**
 * Session editor: the local terminal's fields on the Session page (shell, command line, start folder).
 */
import { useEffect } from 'react';
import { api } from '@/bridge/api';
import { Select, TextInput } from '@/components/ui';
import { useShells } from '@/store/shells';
import { FieldGrid, FormField, PathInput } from '../../common/controls';
import { useEditor } from '../editorContext';

const CUSTOM = '\u0000custom';

export function LocalFields() {
  const { draft, update } = useEditor();
  const shells = useShells((s) => s.shells);
  useEffect(() => void useShells.getState().load(), []);
  const command = draft.local.command.trim();
  const match = (shells ?? []).find((s) => s.command === command);
  const value = !command ? '' : match ? match.id : CUSTOM;
  const options = [
    { value: '', label: 'Default shell' },
    ...(shells ?? []).map((s) => ({ value: s.id, label: s.name })),
    { value: CUSTOM, label: 'Other program…' }
  ];

  return (
    <FieldGrid cols={2}>
      <FormField label="Shell" hint="Installed shells found on this computer">
        <Select
          aria-label="Shell"
          value={value}
          options={options}
          onChange={(id) =>
            update((d) => {
              const shell = shells?.find((s) => s.id === id);
              if (id === CUSTOM) {
                document.getElementById('se-local-cmd')?.focus();  // typed into the command line below
                return;
              }
              d.local.command = shell?.command ?? '';
              // What the shell needs (MSYS2: MSYSTEM, ...), unless the session already sets it.
              for (const v of shell?.environment ?? [])
                if (!d.connection.environment.some((e) => e.name === v.name)) d.connection.environment.push({ ...v });
            })
          }
        />
      </FormField>
      <FormField label="Start folder" hint="Empty: your home folder">
        <PathInput
          value={draft.local.startDir}
          placeholder="~"
          onChange={(v) => update((d) => void (d.local.startDir = v))}
          onBrowse={() => api.dialog.openDirectory({ title: 'Start folder', defaultPath: draft.local.startDir || undefined })}
        />
      </FormField>
      <FormField label="Command line" hint="The program and its arguments; empty: the default shell" span="all" htmlFor="se-local-cmd">
        <TextInput
          id="se-local-cmd"
          className="mono-input"
          value={draft.local.command}
          placeholder="(default shell)"
          onChange={(v) => update((d) => void (d.local.command = v))}
        />
      </FormField>
    </FieldGrid>
  );
}
