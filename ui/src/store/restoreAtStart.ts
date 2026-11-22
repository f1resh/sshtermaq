import { api } from '@/bridge/api';
import { toast } from '@/components/ui';
import { useSessions, useSettings } from '@/store/data';
import { useWorkspace } from '@/store/workspace';
import { restoreWorkspace, type WorkspaceSnapshot } from '@/store/workspacePersist';

/**
 * The tabs at start: after a reload (the page's renderer process died), the old page's tabs, which connect again (the
 * host closed the old connections); otherwise the saved workspace when "Restore tabs on startup" is on.
 */
export async function restoreAtStart(): Promise<void> {
  const settings = useSettings.getState().settings;
  const lookup = (id: string) => useSessions.getState().byId(id);
  const reload = await api.app.takeReloadInfo().catch(() => undefined);
  if (reload?.reloaded) {
    const tabs = reload.workspace
      ? await restoreWorkspace(lookup, settings.defaultSession, true, reload.workspace as WorkspaceSnapshot).catch(() => 0)
      : 0;
    if (reload.closedConnections > 0 || tabs > 0)
      toast.info(
        'The window was reloaded',
        reload.closedConnections > 0
          ? `Its connections were closed${tabs > 0 ? ' and connect again in the restored tabs' : ''}.`
          : 'Its tabs were restored.'
      );
    return;
  }
  if (settings.behavior.restoreTabs && useWorkspace.getState().tabs.length === 0)
    await restoreWorkspace(lookup, settings.defaultSession, settings.behavior.restoreNames).catch(() => 0);
}
