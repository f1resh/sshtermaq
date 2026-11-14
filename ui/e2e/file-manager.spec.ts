import { expect, test, mod } from './fixtures';
import { sessionItem } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * The two-pane file manager against the mock backend: this computer on the left (Windows: C:\Users\demo\Documents,
 * macOS: /Users/demo; src/bridge/mock/local.ts), the server on the right (/home/demo; src/bridge/mock/sftp.ts).
 */
const HOME = '/home/demo';

function panes(page: Page) {
  const local = page.locator('section[data-pane-side="local"]');
  const server = page.locator('section[data-pane-side="remote"]');
  return { local, server };
}
const item = (pane: Locator, path: string) => pane.getByTitle(path, { exact: true });
const nameIn = (pane: Locator, name: string) => pane.locator('.fm-row', { has: pane.page().locator('.fm-name', { hasText: new RegExp(`^${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`) }) });
const queue = (page: Page) => page.locator('.fm-queue');

async function openFileManager(page: Page): Promise<{ local: Locator; server: Locator }> {
  await sessionItem(page, 'web-01').click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Open file manager' }).click();
  const p = panes(page);
  await expect(item(p.server, `${HOME}/notes.txt`)).toBeVisible();
  await expect(p.local.getByRole('grid')).toBeVisible();
  return p;
}

/** The left pane in the Documents folder of the mock computer on both systems. */
async function localDocuments(local: Locator, os: 'win32' | 'darwin'): Promise<string> {
  const docs = os === 'darwin' ? '/Users/demo/Documents' : 'C:\\Users\\demo\\Documents';
  if (os === 'darwin') await item(local, docs).dblclick();
  await expect(local).toHaveAttribute('data-pane-path', docs);
  return docs;
}

test.describe('file manager', () => {
  let local: Locator;
  let server: Locator;
  test.beforeEach(async ({ app }) => {
    ({ local, server } = await openFileManager(app));
  });

  test('navigates the server: open, back, forward, up, Home, a typed path', async ({ app, os }) => {
    await item(server, `${HOME}/projects`).dblclick();
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/projects`);
    await item(server, `${HOME}/projects/sshtermaq`).dblclick();
    await expect(item(server, `${HOME}/projects/sshtermaq/README.md`)).toBeVisible();

    await server.getByRole('button', { name: /^Back/ }).click();
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/projects`);
    await server.getByRole('button', { name: /^Forward/ }).click();
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/projects/sshtermaq`);
    await server.getByRole('button', { name: /^Up/ }).click();
    await server.getByRole('button', { name: /^Up/ }).click();
    await expect(server).toHaveAttribute('data-pane-path', HOME);
    await item(server, `${HOME}/logs`).dblclick();
    await server.getByRole('button', { name: 'Home', exact: true }).click();
    await expect(server).toHaveAttribute('data-pane-path', HOME);

    // Typing a path (Ctrl+L / ⌘L in the list).
    await server.getByRole('grid').click({ position: { x: 5, y: 5 } });
    await app.keyboard.press(`${mod(os)}+L`);
    const box = server.getByRole('textbox', { name: 'Folder' });
    await box.fill('/var/log');
    await box.press('Enter');
    await expect(item(server, '/var/log/syslog')).toBeVisible();
  });

  test('copies with F5 both ways and moves with F6', async ({ app, os }) => {
    const docs = await localDocuments(local, os);
    const sep = os === 'darwin' ? '/' : '\\';

    // Server → computer (F5): the copy stays on the server.
    await item(server, `${HOME}/report.pdf`).click();
    await app.keyboard.press('F5');
    await expect(item(local, `${docs}${sep}report.pdf`)).toBeVisible();
    await expect(item(server, `${HOME}/report.pdf`)).toBeVisible();

    // Computer → server (F5).
    await item(local, `${docs}${sep}quarterly-report.xlsx`).click();
    await app.keyboard.press('F5');
    await expect(item(server, `${HOME}/quarterly-report.xlsx`)).toBeVisible();

    // Move server → computer (F6): gone from the server.
    await item(server, `${HOME}/deploy.sh`).click();
    await app.keyboard.press('F6');
    await expect(item(local, `${docs}${sep}deploy.sh`)).toBeVisible();
    await expect(item(server, `${HOME}/deploy.sh`)).toHaveCount(0);
    await expect(queue(app).locator('.fm-transfer', { hasText: 'deploy.sh' }).locator('.fm-tag')).toHaveText('move');
    await expect(queue(app)).toContainText('All done');
  });

  test('a conflict asks; the toolbar choice "skip" answers without asking', async ({ app, os }) => {
    const docs = await localDocuments(local, os);
    const sep = os === 'darwin' ? '/' : '\\';
    await item(local, `${docs}${sep}notes.txt`).click();
    await app.keyboard.press('F5');
    const conflict = app.getByRole('dialog', { name: '"notes.txt" already exists' });
    await expect(conflict).toBeVisible();
    await expect(conflict).toContainText(`${HOME}/notes.txt`);
    await conflict.getByRole('button', { name: 'Overwrite' }).click();
    await expect(conflict).toBeHidden();
    await expect(queue(app).locator('.fm-transfer--done', { hasText: 'notes.txt' })).toHaveCount(1);

    await app.getByRole('combobox', { name: 'When a file already exists' }).selectOption({ label: 'If a file exists: skip' });
    await item(local, `${docs}${sep}notes.txt`).click();
    await app.keyboard.press('F5');
    await expect(queue(app).locator('.fm-transfer', { hasText: 'notes.txt' }).first()).toContainText('skipped');
    await expect(conflict).toHaveCount(0);
  });

  test('a running transfer can be cancelled and retried; "Clear finished" empties the list', async ({ app }) => {
    // 1.2 GB at the mock's 24 MB/s: running long enough to cancel.
    await item(server, `${HOME}/backup-2026-10-01.tar.gz`).click();
    await app.keyboard.press('F5');
    const transfer = queue(app).locator('.fm-transfer', { hasText: 'backup-2026-10-01.tar.gz' });
    await expect(transfer).toHaveClass(/fm-transfer--running/);
    await expect(queue(app)).toContainText('1 active');
    await transfer.getByRole('button', { name: 'Cancel' }).click();
    await expect(transfer).toHaveClass(/fm-transfer--cancelled/);

    await transfer.getByRole('button', { name: 'Retry' }).click();
    const retried = queue(app).locator('.fm-transfer--running', { hasText: 'backup-2026-10-01.tar.gz' });
    await expect(retried).toHaveCount(1);
    await retried.getByRole('button', { name: 'Cancel' }).click();
    await expect(queue(app).locator('.fm-transfer--running')).toHaveCount(0);

    await queue(app).getByRole('button', { name: 'Clear finished' }).click();
    await expect(queue(app)).toHaveCount(0);
  });

  test('bookmarks: add a folder, go back to it, remove it', async ({ app }) => {
    await item(server, `${HOME}/logs`).dblclick();
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/logs`);
    const star = server.getByRole('button', { name: /^Bookmarks/ });
    await star.click();
    await app.getByRole('menuitem', { name: /^Bookmark this folder/ }).click();
    await expect(star).toHaveAttribute('aria-pressed', 'true');

    await server.getByRole('button', { name: 'Home', exact: true }).click();
    await expect(star).toHaveAttribute('aria-pressed', 'false');
    await star.click();
    await app.getByRole('menuitem', { name: new RegExp(`${HOME}/logs$`) }).click();
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/logs`);

    await star.click();
    await app.getByRole('menuitem', { name: /^Remove this folder/ }).click();
    await expect(star).toHaveAttribute('aria-pressed', 'false');
    await star.click();
    await expect(app.getByRole('menuitem', { name: new RegExp(`${HOME}/logs$`) })).toHaveCount(0);
    await app.keyboard.press('Escape');
  });

  test('new folder (F7), rename in place (F2), delete with the question', async ({ app }) => {
    await server.getByRole('grid').click({ position: { x: 5, y: 5 } });
    await app.keyboard.press('F7');
    const create = app.getByRole('dialog', { name: 'New folder' });
    await create.getByRole('textbox').fill('incoming');
    await create.getByRole('button', { name: 'Create' }).click();
    await expect(item(server, `${HOME}/incoming`)).toBeVisible();

    await item(server, `${HOME}/incoming`).click();
    await app.keyboard.press('F2');
    const field = server.locator('input.fm-rename');
    await expect(field).toHaveValue('incoming');
    await field.fill('incoming-2026');
    await field.press('Enter');
    await expect(item(server, `${HOME}/incoming-2026`)).toBeVisible();

    await item(server, `${HOME}/incoming-2026`).click({ button: 'right' });
    await app.getByRole('menuitem', { name: /^Delete/ }).click();
    const confirm = app.getByRole('dialog', { name: 'Delete "incoming-2026" on the server?' });
    await confirm.getByRole('button', { name: 'Delete' }).click();
    await expect(item(server, `${HOME}/incoming-2026`)).toHaveCount(0);
  });

  test('find files on the server, and go to a result', async ({ app }) => {
    await app.getByRole('button', { name: 'Find', exact: true }).click();
    const find = app.getByRole('dialog', { name: 'Find files on the server' });
    await find.getByPlaceholder('*.log; config*').fill('app-2026-09-0?.log');
    await find.getByRole('button', { name: 'Search' }).click();
    await expect(find).toContainText(/\d+ found in \d+ folders?/);
    const hit = find.getByTitle(`${HOME}/logs/app-2026-09-01.log`, { exact: true });
    await expect(hit).toBeVisible();
    await expect(find.locator('.fm-find__row')).toHaveCount(9);   // 01 to 09
    await hit.dblclick();
    await expect(find).toBeHidden();
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/logs`);

    // Nothing found.
    await app.getByRole('button', { name: 'Find', exact: true }).click();
    await find.getByPlaceholder('*.log; config*').fill('*.nothing-like-this');
    await find.getByRole('button', { name: 'Search' }).click();
    await expect(find).toContainText('No matches.');
    await find.getByRole('button', { name: 'Close' }).last().click();
    await expect(find).toBeHidden();
  });

  test('synchronize: compare shows the differences, then runs them as transfers', async ({ app, os }) => {
    await localDocuments(local, os);
    await app.getByRole('button', { name: 'Synchronize' }).click();
    const sync = app.getByRole('dialog', { name: 'Synchronize folders' });
    await sync.getByRole('radiogroup', { name: 'Direction' }).getByText(/^Server ←/).click();
    await sync.getByRole('button', { name: 'Compare' }).click();
    await expect(sync.locator('.fm-sync__totals')).toContainText(/\d+ uploads?/);
    await expect(sync.locator('.fm-sync__list')).toContainText('quarterly-report.xlsx');
    await sync.getByRole('button', { name: 'Synchronize' }).click();
    await expect(sync).toBeHidden();
    await expect(queue(app).locator('.fm-tag', { hasText: 'sync' }).first()).toBeVisible();
    await expect(queue(app)).toContainText('All done', { timeout: 15000 });
    await expect(item(server, `${HOME}/quarterly-report.xlsx`)).toBeVisible();
  });

  test('hidden files on request; a terminal to the same server from the toolbar', async ({ app }) => {
    await expect(item(server, `${HOME}/.bashrc`)).toHaveCount(0);
    await app.getByRole('button', { name: 'Show hidden files' }).click();
    await expect(item(server, `${HOME}/.bashrc`)).toBeVisible();
    await app.getByRole('button', { name: 'Hide hidden files' }).click();
    await expect(item(server, `${HOME}/.bashrc`)).toHaveCount(0);

    await app.getByRole('button', { name: 'Open a terminal to this server' }).click();
    await expect(app.locator('.term-pane')).toHaveCount(1);
    await expect(app.getByRole('tab', { name: /web-01/ })).toHaveCount(2);
  });

  test('the local side lists the computer: "This PC" / the home folder, and filters', async ({ os }) => {
    if (os === 'win32') {
      await local.getByRole('button', { name: /^Up/ }).click();     // C:\Users\demo
      await local.getByRole('button', { name: /^Up/ }).click();     // C:\Users
      await local.getByRole('button', { name: /^Up/ }).click();     // C:\
      await local.getByRole('button', { name: /^Up/ }).click();     // This PC: the drives
      await expect(local).toHaveAttribute('data-pane-path', '');
      await expect(nameIn(local, 'C:').or(local.getByText(/\(C:\)/)).first()).toBeVisible();
    } else {
      await expect(item(local, '/Users/demo/Documents')).toBeVisible();
    }
    // The filter narrows the list.
    await local.getByRole('button', { name: 'Filter' }).click();
    const filter = local.locator('.fm-filter__input');
    await filter.fill('zzz-no-such');
    await expect(local).toContainText('Nothing matches the filter.');
    await local.getByRole('button', { name: 'Clear filter' }).click();
    await expect(local).not.toContainText('Nothing matches the filter.');
  });
  test('macOS: the Finder keys work in a file list', async ({ app, os }) => {
    test.skip(os !== 'darwin', 'Command and Finder keys are the macOS layout; Windows uses Ctrl (listKeys.test.ts)');
    const grid = server.getByRole('grid');
    // ⌘↓ opens the folder under the cursor, ⌘↑ goes up.
    await item(server, `${HOME}/projects`).click();
    await app.keyboard.press('Meta+ArrowDown');
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/projects`);
    await app.keyboard.press('Meta+ArrowUp');
    await expect(server).toHaveAttribute('data-pane-path', HOME);
    // ⌘[ back, ⌘] forward.
    await app.keyboard.press('Meta+BracketLeft');
    await expect(server).toHaveAttribute('data-pane-path', `${HOME}/projects`);
    await app.keyboard.press('Meta+BracketRight');
    await expect(server).toHaveAttribute('data-pane-path', HOME);

    // ⇧⌘N makes a folder, ⌘⌫ deletes it (with the question).
    await grid.click({ position: { x: 5, y: 5 } });
    await app.keyboard.press('Shift+Meta+KeyN');
    const create = app.getByRole('dialog', { name: 'New folder' });
    await create.getByRole('textbox').fill('from-keys');
    await create.getByRole('button', { name: 'Create' }).click();
    await expect(item(server, `${HOME}/from-keys`)).toBeVisible();
    await item(server, `${HOME}/from-keys`).click();
    await app.keyboard.press('Meta+Backspace');
    const confirm = app.getByRole('dialog', { name: 'Delete "from-keys" on the server?' });
    await confirm.getByRole('button', { name: 'Delete' }).click();
    await expect(item(server, `${HOME}/from-keys`)).toHaveCount(0);

    // ⇧⌘G types a path, as Finder's "Go to Folder".
    await grid.click({ position: { x: 5, y: 5 } });
    await app.keyboard.press('Shift+Meta+KeyG');
    const box = server.getByRole('textbox', { name: 'Folder' });
    await box.fill('/var/log');
    await box.press('Enter');
    await expect(item(server, '/var/log/syslog')).toBeVisible();
  });

  test('macOS: above the home folder are "/" and Computer with the volumes; ~ in a typed path', async ({ app, os }) => {
    test.skip(os !== 'darwin', 'the Windows layout climbs to This PC in "the local side lists the computer"');
    const up = local.getByRole('button', { name: /^Up/ });
    await expect(local).toHaveAttribute('data-pane-path', '/Users/demo');
    await up.click();
    await expect(local).toHaveAttribute('data-pane-path', '/Users');
    await up.click();
    await expect(local).toHaveAttribute('data-pane-path', '/');
    await up.click();
    await expect(local).toHaveAttribute('data-pane-path', '');   // Computer: the places and the volumes
    await expect(local.getByText('Macintosh HD')).toBeVisible();
    await expect(local.getByText('Data', { exact: true })).toBeVisible();
    await expect(up).toBeDisabled();

    // A volume's root goes up to Computer, not to /Volumes.
    await local.getByText('Data', { exact: true }).dblclick();
    await expect(local).toHaveAttribute('data-pane-path', '/Volumes/Data');
    await expect(item(local, '/Volumes/Data/Backups')).toBeVisible();
    await up.click();
    await expect(local).toHaveAttribute('data-pane-path', '');

    // "~" in a typed path is the home folder.
    await local.getByRole('grid').click({ position: { x: 5, y: 5 } });
    await app.keyboard.press('Meta+KeyL');
    const box = local.getByRole('textbox', { name: 'Folder' });
    await box.fill('~/Documents');
    await box.press('Enter');
    await expect(local).toHaveAttribute('data-pane-path', '/Users/demo/Documents');
  });

  test('local files use the words of the system: Trash or Recycle Bin, Finder or Explorer', async ({ app, os }) => {
    const docs = await localDocuments(local, os);
    const sep = os === 'darwin' ? '/' : '\\';
    const notes = item(local, `${docs}${sep}notes.txt`);
    await notes.click({ button: 'right' });
    await expect(app.getByRole('menuitem', { name: os === 'darwin' ? 'Show in Finder' : 'Show in Explorer' })).toBeVisible();
    await app.getByRole('menuitem', { name: /^Delete/ }).click();
    const confirm = app.getByRole('dialog', { name: 'Delete "notes.txt"?' });
    await expect(confirm).toContainText(os === 'darwin' ? 'They go to the Trash.' : 'They go to the Recycle Bin.');
    await confirm.getByRole('button', { name: 'Cancel' }).click();
    await expect(notes).toBeVisible();
  });
});
