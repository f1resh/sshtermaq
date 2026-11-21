/**
 * Generic confirmation dialog (useUi dialog type 'confirm', see confirmDialog() in store/ui.ts).
 */
import { useState } from 'react';
import { AlertTriangle, HelpCircle } from 'lucide-react';
import { Button, Checkbox, Modal } from '@/components/ui';
import type { DialogState } from '@/store/ui';
import { useInitialFocus, useStableCallback } from './common/hooks';

export type ConfirmDialogState = Extract<DialogState, { type: 'confirm' }>;

export interface ConfirmDialogProps {
  dialog: ConfirmDialogState;
  /** true = confirmed, false = cancelled / dismissed; `option`: the checkbox, when there is one. */
  onResolve: (confirmed: boolean, option: boolean) => void;
}

export function ConfirmDialog({ dialog, onResolve }: ConfirmDialogProps) {
  const [option, setOption] = useState(false);
  const cancel = useStableCallback(() => onResolve(false, false));
  // Danger confirmations focus the safe choice by default.
  const cancelRef = useInitialFocus<HTMLButtonElement>(!!dialog.danger);
  const okRef = useInitialFocus<HTMLButtonElement>(!dialog.danger);
  return (
    <Modal
      open
      title={dialog.title}
      onClose={cancel}
      width={460}
      icon={
        dialog.danger ? (
          <AlertTriangle size={18} className="dlg-title-icon dlg-title-icon--danger" />
        ) : (
          <HelpCircle size={18} className="dlg-title-icon" />
        )
      }
      footer={
        <>
          <Button ref={cancelRef} onClick={cancel}>
            Cancel
          </Button>
          <Button ref={okRef} variant={dialog.danger ? 'danger' : 'primary'} onClick={() => onResolve(true, option)}>
            {dialog.confirmLabel ?? 'OK'}
          </Button>
        </>
      }
    >
      <div className="dlg-confirm__message">{dialog.message}</div>
      {dialog.details && <pre className="dlg-confirm__details mono-text">{dialog.details}</pre>}
      {dialog.option && <Checkbox className="dlg-confirm__option" checked={option} onChange={setOption} label={dialog.option} />}
    </Modal>
  );
}
