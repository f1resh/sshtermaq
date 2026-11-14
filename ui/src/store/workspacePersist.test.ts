import { describe, expect, it } from 'vitest';
import { createDefaultSession, createLocalSession } from '@shared/defaults';
import { deserializeWorkspace, serializeWorkspace } from './workspacePersist';
import { connIdOf, type PaneState, type TabState } from './workspace';

function pane(id: string, tabId: string, extra: Partial<PaneState> = {}): PaneState {
  return {
    id,
    tabId,
    kind: 'terminal',
    sessionId: null,
    session: createDefaultSession({ host: `${id}.example.test` }),
    generation: 1,
    connId: connIdOf(id, 1),
    status: 'connected',
    hasActivity: false,
    bell: false,
    ...extra
  };
}

describe('workspace persistence with file manager tabs', () => {
  it('keeps the kind and folders, drops the terminal link', () => {
    const panes: Record<string, PaneState> = {
      t1: pane('t1', 'A'),
      f1: pane('f1', 'B', { kind: 'files', linkedPaneId: 't1', files: { localPath: 'C:\\work', remotePath: '/srv/www' } })
    };
    const tabs: TabState[] = [
      { id: 'A', layout: { type: 'pane', paneId: 't1' }, activePaneId: 't1', broadcast: false },
      { id: 'B', layout: { type: 'pane', paneId: 'f1' }, activePaneId: 'f1', broadcast: false }
    ];
    const snap = serializeWorkspace({ tabs, panes, activeTabId: 'B' }, () => false);
    expect(snap.activeTab).toBe(1);
    const filesNode = snap.tabs[1].layout;
    expect(filesNode.type === 'pane' && filesNode.pane.kind).toBe('files');

    const restored = deserializeWorkspace(JSON.parse(JSON.stringify(snap)), () => undefined);
    expect(restored.tabs).toHaveLength(2);
    const restoredFiles = Object.values(restored.panes).find((p) => p.kind === 'files')!;
    expect(restoredFiles.files).toEqual({ localPath: 'C:\\work', remotePath: '/srv/www' });
    expect(restoredFiles.linkedPaneId).toBeUndefined();
    expect(restoredFiles.status).toBe('idle');
    expect(Object.values(restored.panes).filter((p) => p.kind === 'terminal')).toHaveLength(1);
  });

  it('treats old snapshots as terminals', () => {
    const restored = deserializeWorkspace(
      { version: 1, activeTab: 0, tabs: [{ layout: { type: 'pane', pane: { sessionId: null, session: createDefaultSession({ host: 'h' }) } }, activePane: 0 }] },
      () => undefined
    );
    expect(Object.values(restored.panes)[0].kind).toBe('terminal');
  });
});

describe('workspace persistence with local terminals', () => {
  it('brings local terminals back waiting to be started, other sessions as before', () => {
    const command = String.raw`"C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo`;
    const local = createLocalSession({ id: 'pwsh', name: 'PowerShell 7', command, environment: [], isDefault: true });
    const panes: Record<string, PaneState> = {
      l1: pane('l1', 'A', { session: local }),
      s1: pane('s1', 'B')
    };
    const tabs: TabState[] = [
      { id: 'A', layout: { type: 'pane', paneId: 'l1' }, activePaneId: 'l1', broadcast: false },
      { id: 'B', layout: { type: 'pane', paneId: 's1' }, activePaneId: 's1', broadcast: false }
    ];
    const snap = serializeWorkspace({ tabs, panes, activeTabId: 'A' }, () => false);
    const restored = deserializeWorkspace(JSON.parse(JSON.stringify(snap)), () => undefined);
    const all = Object.values(restored.panes);
    const restoredLocal = all.find((p) => p.session.protocol === 'local')!;
    expect(restoredLocal.deferStart).toBe(true);
    expect(restoredLocal.status).toBe('idle');
    expect(restoredLocal.session.local.command).toBe(command);
    expect(all.find((p) => p.session.protocol === 'ssh')!.deferStart).toBeUndefined();
  });
});

describe('workspace persistence of names', () => {
  /** One split tab named "Production", its first pane named "logs", saved as on quit. */
  function savedNamedTab() {
    const panes: Record<string, PaneState> = {
      a: pane('a', 'A', { customTitle: 'logs' }),
      b: pane('b', 'A')
    };
    const tabs: TabState[] = [
      {
        id: 'A',
        layout: { type: 'split', id: 's', direction: 'row', sizes: [0.5, 0.5], children: [{ type: 'pane', paneId: 'a' }, { type: 'pane', paneId: 'b' }] },
        activePaneId: 'a',
        broadcast: false,
        customTitle: 'Production'
      }
    ];
    return JSON.parse(JSON.stringify(serializeWorkspace({ tabs, panes, activeTabId: 'A' }, () => false)));
  }

  it('"Keep tab and pane names" off: the tabs come back with automatic titles, the snapshot keeps the names', () => {
    const snap = savedNamedTab();
    const restored = deserializeWorkspace(snap, () => undefined, undefined, false);
    expect(restored.tabs).toHaveLength(1);
    expect(restored.tabs[0].customTitle).toBeUndefined();
    expect(Object.values(restored.panes).map((p) => p.customTitle)).toEqual([undefined, undefined]);
    // Turned back on later: the names are still there.
    const again = deserializeWorkspace(snap, () => undefined, undefined, true);
    expect(again.tabs[0].customTitle).toBe('Production');
  });

  it('tab and pane names come back after a restart', () => {
    const panes: Record<string, PaneState> = {
      a: pane('a', 'A', { customTitle: 'logs' }),
      b: pane('b', 'A')
    };
    const tabs: TabState[] = [
      {
        id: 'A',
        layout: { type: 'split', id: 's', direction: 'row', sizes: [0.5, 0.5], children: [{ type: 'pane', paneId: 'a' }, { type: 'pane', paneId: 'b' }] },
        activePaneId: 'a',
        broadcast: false,
        customTitle: 'Production'
      }
    ];
    const snap = serializeWorkspace({ tabs, panes, activeTabId: 'A' }, () => false);
    const restored = deserializeWorkspace(JSON.parse(JSON.stringify(snap)), () => undefined);
    expect(restored.tabs[0].customTitle).toBe('Production');
    const names = Object.values(restored.panes).map((p) => p.customTitle);
    expect(names).toContain('logs');
    expect(names.filter((n) => n === undefined)).toHaveLength(1);
  });
});
