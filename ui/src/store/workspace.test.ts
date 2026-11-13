import { beforeEach, describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';
import { connIdOf, paneIdsOf, paneTitle, tabTitle, useWorkspace, type PaneState, type TabState } from './workspace';

function pane(extra: Partial<PaneState> = {}, session: Partial<SessionConfig> = {}): PaneState {
  const s = createDefaultSession({ name: 'web-01', host: '10.0.0.5', username: 'deploy', ...session });
  s.terminal.allowRemoteTitle = true;
  return {
    id: 'p1',
    tabId: 'T1',
    kind: 'terminal',
    sessionId: null,
    session: s,
    generation: 1,
    connId: connIdOf('p1', 1),
    status: 'connected',
    hasActivity: false,
    bell: false,
    remoteTitle: 'vim notes.txt',
    ...extra
  };
}

const tab = (activePaneId: string, customTitle?: string): TabState => ({
  id: 'T1',
  layout: { type: 'pane', paneId: activePaneId },
  activePaneId,
  broadcast: false,
  customTitle
});

describe('tab titles', () => {
  it('terminal mode: the title the server set, else the session name', () => {
    expect(paneTitle(pane(), 'terminal')).toBe('vim notes.txt');
    expect(paneTitle(pane({ remoteTitle: undefined }), 'terminal')).toBe('web-01');
    const noRemote = pane();
    noRemote.session.terminal.allowRemoteTitle = false;
    expect(paneTitle(noRemote, 'terminal')).toBe('web-01');
  });

  it('session mode: the session name; unsaved sessions show user@host[:port]', () => {
    expect(paneTitle(pane(), 'session')).toBe('web-01');
    expect(paneTitle(pane({}, { name: '', port: 2222 }), 'session')).toBe('deploy@10.0.0.5:2222');
  });

  it('host mode: the host name or address; the line for serial sessions', () => {
    expect(paneTitle(pane(), 'host')).toBe('10.0.0.5');
    const serial = pane({}, { protocol: 'serial' });
    serial.session.serial.line = 'COM3';
    expect(paneTitle(serial, 'host')).toBe('COM3');
  });

  it("a session's fixed window title and a renamed tab always win", () => {
    const fixed = pane();
    fixed.session.terminal.windowTitle = 'Production DB';
    for (const mode of ['terminal', 'session', 'host'] as const) {
      expect(paneTitle(fixed, mode)).toBe('Production DB');
      expect(tabTitle(tab('p1', 'mine'), { p1: fixed }, mode)).toBe('mine');
    }
  });

  it('file manager tabs: the session name, or the host in host mode', () => {
    const files = pane({ kind: 'files' });
    expect(paneTitle(files, 'terminal')).toBe('web-01');
    expect(paneTitle(files, 'host')).toBe('10.0.0.5');
  });

  it('defaults to terminal mode and uses the active pane', () => {
    expect(tabTitle(tab('p1'), { p1: pane() })).toBe('vim notes.txt');
    expect(tabTitle(tab('missing'), { p1: pane() })).toBe('Tab');
  });
});

describe('closing panes of a split tab', () => {
  beforeEach(() => useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null }));

  const openSplit = () => {
    const first = useWorkspace.getState().openSession(createDefaultSession({ host: 'a.example.test' }));
    const second = useWorkspace.getState().splitPane(first, 'row');
    return { first, second };
  };

  it('keeps the tab and makes the remaining pane active', () => {
    const { first, second } = openSplit();
    expect(paneIdsOf(useWorkspace.getState().tabs[0].layout)).toEqual([first, second]);
    useWorkspace.getState().activatePane(second);

    useWorkspace.getState().closePane(second);
    const st = useWorkspace.getState();
    expect(st.tabs).toHaveLength(1);
    expect(st.tabs[0].layout).toEqual({ type: 'pane', paneId: first });
    expect(st.tabs[0].activePaneId).toBe(first);
    expect(st.panes[second]).toBeUndefined();
  });

  it('keeps the active pane when another one closes', () => {
    const { first, second } = openSplit();
    useWorkspace.getState().activatePane(second);
    useWorkspace.getState().closePane(first);
    expect(useWorkspace.getState().tabs[0].activePaneId).toBe(second);
  });

  it('closes the tab with its last pane', () => {
    const { first, second } = openSplit();
    useWorkspace.getState().closePane(first);
    useWorkspace.getState().closePane(second);
    expect(useWorkspace.getState().tabs).toHaveLength(0);
    expect(useWorkspace.getState().panes).toEqual({});
  });
});

describe('renaming tabs and panes', () => {
  beforeEach(() => useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null }));
  const ws = () => useWorkspace.getState();
  const open = () => ws().openSession(createDefaultSession({ name: 'web-01', host: 'a.example.test' }));

  it('a tab name replaces the automatic title; clearing it brings the automatic title back', () => {
    const p = open();
    const tabId = ws().panes[p].tabId;
    ws().renameTab(tabId, 'Production');
    expect(tabTitle(ws().tabs[0], ws().panes)).toBe('Production');
    ws().renameTab(tabId, '');
    expect(ws().tabs[0].customTitle).toBeUndefined();
    expect(tabTitle(ws().tabs[0], ws().panes, 'session')).toBe('web-01');
  });

  it('a pane name shows in its header and, without a tab name, on the tab', () => {
    const first = open();
    const second = ws().splitPane(first, 'row')!;
    ws().renamePane(second, '  logs  ');
    expect(ws().panes[second].customTitle).toBe('logs');
    expect(paneTitle(ws().panes[second], 'terminal')).toBe('logs');
    ws().activatePane(second);
    expect(tabTitle(ws().tabs[0], ws().panes)).toBe('logs');
    ws().renameTab(ws().tabs[0].id, 'Prod');
    expect(tabTitle(ws().tabs[0], ws().panes)).toBe('Prod');  // the tab's own name wins
    ws().renamePane(second, '');
    expect(ws().panes[second].customTitle).toBeUndefined();
  });

  it('a pane name wins over a fixed window title and the title the server set', () => {
    const p = pane({ customTitle: 'mine' });
    p.session.terminal.windowTitle = 'Production DB';
    for (const mode of ['terminal', 'session', 'host'] as const) expect(paneTitle(p, mode)).toBe('mine');
  });

  it('duplicating a tab keeps its names; splitting a pane does not copy the pane name', () => {
    const p = open();
    ws().renameTab(ws().panes[p].tabId, 'Prod');
    ws().renamePane(p, 'shell');
    const copy = ws().duplicatePane(p, 'tab')!;
    const copyTab = ws().tabs.find((t) => t.id === ws().panes[copy].tabId)!;
    expect(copyTab.id).not.toBe(ws().panes[p].tabId);
    expect(copyTab.customTitle).toBe('Prod');
    expect(ws().panes[copy].customTitle).toBe('shell');

    const split = ws().duplicatePane(p, 'split-right')!;
    expect(ws().panes[split].tabId).toBe(ws().panes[p].tabId);
    expect(ws().panes[split].customTitle).toBeUndefined();
  });
});
