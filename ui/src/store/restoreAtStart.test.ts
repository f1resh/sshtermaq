import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createDefaultSession, createDefaultSettings } from '@shared/defaults';
import type { PageReloadInfo } from '@shared/types';
import { api } from '@/bridge/api';
import { useSettings } from '@/store/data';
import { useWorkspace } from '@/store/workspace';
import { serializeWorkspace, startWorkspaceAutosave, type WorkspaceSnapshot } from './workspacePersist';

const toastInfo = vi.fn();
vi.mock('@/components/ui', () => ({ toast: { info: (...a: unknown[]) => toastInfo(...a) } }));

const { restoreAtStart } = await import('./restoreAtStart');

/** A snapshot of one tab with an SSH session, as the old page sent it. */
function snapshotOf(host: string): WorkspaceSnapshot {
  useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null });
  useWorkspace.getState().openSession(createDefaultSession({ host }));
  const snap = serializeWorkspace(useWorkspace.getState(), () => false);
  useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null });
  return JSON.parse(JSON.stringify(snap)) as WorkspaceSnapshot;
}

const hosts = () => Object.values(useWorkspace.getState().panes).map((p) => p.session.host);

function withRestoreTabs(restoreTabs: boolean) {
  const settings = createDefaultSettings();
  settings.behavior.restoreTabs = restoreTabs;
  useSettings.setState({ settings });
}

describe('tabs at start', () => {
  let reloadInfo: ReturnType<typeof vi.spyOn>;
  let getState: ReturnType<typeof vi.spyOn>;
  const saved = (host: string) => getState.mockResolvedValue(snapshotOf(host) as never);
  const reload = (info: PageReloadInfo) => reloadInfo.mockResolvedValue(info);

  beforeEach(() => {
    toastInfo.mockReset();
    useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null });
    reloadInfo = vi.spyOn(api.app, 'takeReloadInfo');
    getState = vi.spyOn(api.app, 'getState').mockResolvedValue(undefined as never);
  });
  afterEach(() => vi.restoreAllMocks());

  it('after a reload: the old page tabs come back, also with "Restore tabs on startup" off, and connect again', async () => {
    withRestoreTabs(false);
    saved('saved-on-disk.example.test');
    reload({ reloaded: true, closedConnections: 2, workspace: snapshotOf('live.example.test') });
    await restoreAtStart();
    expect(hosts()).toEqual(['live.example.test']);
    expect(Object.values(useWorkspace.getState().panes)[0].status).toBe('idle');  // a new connection, not the old one
    expect(getState).not.toHaveBeenCalled();  // not the workspace saved on disk
    expect(toastInfo).toHaveBeenCalledWith('The window was reloaded', 'Its connections were closed and connect again in the restored tabs.');
  });

  it('after a reload with "Restore tabs on startup" on: the old page tabs, not the saved ones', async () => {
    withRestoreTabs(true);
    saved('saved-on-disk.example.test');
    reload({ reloaded: true, closedConnections: 1, workspace: snapshotOf('live.example.test') });
    await restoreAtStart();
    expect(hosts()).toEqual(['live.example.test']);
  });

  it('after a reload without a workspace: says the connections were closed', async () => {
    withRestoreTabs(false);
    reload({ reloaded: true, closedConnections: 2 });
    await restoreAtStart();
    expect(hosts()).toEqual([]);
    expect(toastInfo).toHaveBeenCalledWith('The window was reloaded', 'Its connections were closed.');
  });

  it('after a reload of an empty window: nothing to say', async () => {
    withRestoreTabs(false);
    reload({ reloaded: true, closedConnections: 0 });
    await restoreAtStart();
    expect(toastInfo).not.toHaveBeenCalled();
  });

  it('a normal start restores the saved workspace when the setting is on, as before', async () => {
    withRestoreTabs(true);
    saved('saved-on-disk.example.test');
    reload({ reloaded: false, closedConnections: 0 });
    await restoreAtStart();
    expect(hosts()).toEqual(['saved-on-disk.example.test']);
    expect(toastInfo).not.toHaveBeenCalled();
  });

  it('a normal start with the setting off opens no tabs, as before', async () => {
    withRestoreTabs(false);
    saved('saved-on-disk.example.test');
    reload({ reloaded: false, closedConnections: 0 });
    await restoreAtStart();
    expect(hosts()).toEqual([]);
    expect(getState).not.toHaveBeenCalled();
  });

  it('a host that cannot tell (the call fails) starts as before', async () => {
    withRestoreTabs(true);
    saved('saved-on-disk.example.test');
    reloadInfo.mockRejectedValue(new Error('Not implemented: app:takeReloadInfo'));
    await restoreAtStart();
    expect(hosts()).toEqual(['saved-on-disk.example.test']);
  });
});

describe('workspace autosave and the reload handover', () => {
  let handover: ReturnType<typeof vi.spyOn>;
  let setState: ReturnType<typeof vi.spyOn>;
  let stop: () => void;

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal('window', { addEventListener: () => undefined, removeEventListener: () => undefined });
    useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null });
    handover = vi.spyOn(api.app, 'setReloadSnapshot').mockImplementation(() => undefined);
    setState = vi.spyOn(api.app, 'setState').mockResolvedValue(undefined);
  });
  afterEach(() => {
    stop?.();
    vi.useRealTimers();
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  const openTab = (host: string) => useWorkspace.getState().openSession(createDefaultSession({ host }));

  it('with restoring tabs off: every change goes to the host memory only, never to disk', () => {
    stop = startWorkspaceAutosave(() => false, () => false, 100);
    openTab('a.example.test');
    vi.advanceTimersByTime(150);
    expect(handover).toHaveBeenCalledTimes(1);
    const sent = handover.mock.calls[0][0] as WorkspaceSnapshot;
    expect(JSON.stringify(sent)).toContain('a.example.test');
    expect(setState).not.toHaveBeenCalled();

    openTab('b.example.test');
    vi.advanceTimersByTime(150);
    expect(handover).toHaveBeenCalledTimes(2);
    expect(setState).not.toHaveBeenCalled();
  });

  it('with restoring tabs on: saved to disk as before, and to the host memory', () => {
    stop = startWorkspaceAutosave(() => true, () => false, 100);
    openTab('a.example.test');
    vi.advanceTimersByTime(150);
    expect(setState).toHaveBeenCalledTimes(1);
    expect(handover).toHaveBeenCalledTimes(1);
    // A status change is not a structural change: nothing is sent again.
    const paneId = Object.keys(useWorkspace.getState().panes)[0];
    useWorkspace.setState((s) => ({ panes: { ...s.panes, [paneId]: { ...s.panes[paneId], status: 'connected' } } }));
    vi.advanceTimersByTime(150);
    expect(setState).toHaveBeenCalledTimes(1);
    expect(handover).toHaveBeenCalledTimes(1);
  });
});
