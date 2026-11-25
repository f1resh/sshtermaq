/**
 * "Update" chip in the title bar when a newer version is out (src/store/updates.ts): next to the caption buttons on
 * Windows, after the traffic lights on macOS. A click opens its menu: the release page, the upgrade command where
 * there is one (Homebrew), "Remind me tomorrow" and "Skip this version".
 */
import { CircleArrowUp } from 'lucide-react';
import { api } from '@/bridge/api';
import { showContextMenu, toast, type MenuItem } from '@/components/ui';
import { openReleasePage, remindTomorrow, skipVersion, useOffer, useUpdates } from '@/store/updates';
import { upgradeCommand } from '@/store/updatesModel';

export function UpdateChip() {
  const offer = useOffer();
  const current = useUpdates((s) => s.current);
  const channel = useUpdates((s) => s.channel);
  if (!offer) return null;
  const text = `SSHTermaq ${offer.version} is available (you have ${current})`;
  const command = upgradeCommand(channel);
  const menu = (): MenuItem[] => [
    { type: 'header', label: `Version ${offer.version} is available` },  // the full sentence is the chip's tooltip
    { label: 'Open release page', onClick: () => openReleasePage(offer) },
    ...(command
      ? [
          {
            label: `Copy “${command}”`,
            onClick: () => {
              api.app.writeClipboard(command);
              toast.info('Copied', `Run ${command} in Terminal.`);
            }
          }
        ]
      : []),
    { type: 'separator' },
    { label: 'Remind me tomorrow', onClick: remindTomorrow },
    { label: 'Skip this version', onClick: () => skipVersion(offer.version) }
  ];
  return (
    <button
      type="button"
      className="titlebar__update no-drag"
      title={text}
      aria-label={text}
      aria-haspopup="menu"
      onClick={(e) => {
        const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
        showContextMenu(r.left, r.bottom + 2, menu());
      }}
    >
      <CircleArrowUp size={13} />
      <span>Update</span>
    </button>
  );
}
