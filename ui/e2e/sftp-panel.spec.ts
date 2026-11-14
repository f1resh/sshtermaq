import { expect, test } from './fixtures';
import { mockDialog, openSftpPanel, sessionItem, updateSettings } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * The SFTP panel beside an SSH terminal, against the mock server (src/bridge/mock/sftp.ts): its home is /home/demo
 * with .bashrc (hidden), notes.txt, deploy.sh (755), report.pdf, the folders logs, projects, Downloads, ...
 */
const HOME = '/home/demo';
const row = (panel: Locator, path: string) => panel.getByTitle(path, { exact: true });
/** Where a right-click gets the folder's menu (not an item's): the list's header row. */
const listArea = (panel: Locator) => panel.locator('.sftp__row--head');
const pathBox = (page: Page) => page.getByRole('textbox', { name: 'Path' });
/** The local file the mock's file pickers can point at (src/bridge/mock/local.ts). */
const localNotes = (os: 'win32' | 'darwin') => (os === 'darwin' ? '/Users/demo/Documents/notes.txt' : 'C:\\Users\\demo\\Documents\\notes.txt');

async function contextMenu(page: Page, target: Locator, item: string | RegExp): Promise<void> {
  await target.click({ button: 'right' });
  await page.getByRole('menuitem', { name: item }).click();
}

test.describe('SFTP panel', () => {
  let panel: Locator;
  test.beforeEach(async ({ app }) => {
    panel = await openSftpPanel(app);
    await expect(row(panel, `${HOME}/notes.txt`)).toBeVisible();
  });

  test('navigates: open a folder, up, Home, breadcrumbs, a typed path, Backspace', async ({ app }) => {
    await row(panel, `${HOME}/projects`).dblclick();
    await expect(pathBox(app)).toHaveValue(`${HOME}/projects`);
    await row(panel, `${HOME}/projects/sshtermaq`).dblclick();
    await expect(row(panel, `${HOME}/projects/sshtermaq/README.md`)).toBeVisible();

    await panel.getByRole('button', { name: 'Up (Backspace)' }).click();
    await expect(pathBox(app)).toHaveValue(`${HOME}/projects`);
    await panel.getByRole('button', { name: 'Home', exact: true }).click();
    await expect(pathBox(app)).toHaveValue(HOME);

    await panel.locator('.sftp__crumb', { hasText: 'home' }).click();
    await expect(pathBox(app)).toHaveValue('/home');

    await pathBox(app).fill('/var/log');
    await pathBox(app).press('Enter');
    await expect(row(panel, '/var/log/syslog')).toBeVisible();

    await row(panel, '/var/log/syslog').click();
    await app.keyboard.press('Backspace');
    await expect(pathBox(app)).toHaveValue('/var');

    // A folder that does not exist: the error is shown and the path box goes back.
    await pathBox(app).fill('/no/such/folder');
    await pathBox(app).press('Enter');
    await expect(panel.locator('.sftp__error')).toBeVisible();
    await expect(pathBox(app)).toHaveValue('/var');
  });

  test('hidden files are shown on request; columns sort both ways', async () => {
    await expect(row(panel, `${HOME}/.bashrc`)).toHaveCount(0);
    await panel.getByRole('button', { name: 'Show hidden files' }).click();
    await expect(row(panel, `${HOME}/.bashrc`)).toBeVisible();
    await panel.getByRole('button', { name: 'Hide hidden files' }).click();
    await expect(row(panel, `${HOME}/.bashrc`)).toHaveCount(0);

    const names = () => panel.locator('.sftp__row:not(.sftp__row--head) .sftp__name').allInnerTexts();
    const head = panel.locator('.sftp__row--head');
    await head.getByRole('button', { name: /^Size/ }).click();
    const bySize = await names();
    await head.getByRole('button', { name: /^Size/ }).click();
    const bySizeDown = await names();
    expect(bySizeDown).not.toEqual(bySize);
    // The biggest file of the home folder comes first when sorting down by size.
    expect(bySizeDown.filter((n) => n.includes('.'))[0]).toBe('backup-2026-10-01.tar.gz');

    await head.getByRole('button', { name: /^Name/ }).click();
    const byName = await names();
    const files = byName.filter((n) => ['deploy.sh', 'notes.txt', 'report.pdf'].includes(n));
    expect(files).toEqual(['deploy.sh', 'notes.txt', 'report.pdf']);
  });

  test('new folder, rename with F2, delete with and without the question', async ({ app }) => {
    await panel.getByRole('button', { name: 'New folder' }).click();
    const create = app.getByRole('dialog', { name: 'New folder' });
    await create.getByRole('textbox').fill('a/b');
    await expect(create).toContainText('/');           // a name, not a path
    await create.getByRole('textbox').fill('reports');
    await create.getByRole('button', { name: 'Create' }).click();
    await expect(row(panel, `${HOME}/reports`)).toBeVisible();

    await row(panel, `${HOME}/reports`).click();
    await app.keyboard.press('F2');
    const rename = app.getByRole('dialog', { name: 'Rename' });
    await expect(rename.getByRole('textbox')).toHaveValue('reports');
    await rename.getByRole('textbox').fill('reports-2026');
    await rename.getByRole('button', { name: 'Rename' }).click();
    await expect(row(panel, `${HOME}/reports-2026`)).toBeVisible();
    await expect(row(panel, `${HOME}/reports`)).toHaveCount(0);

    await row(panel, `${HOME}/reports-2026`).click();
    await app.keyboard.press('Delete');
    const confirm = app.getByRole('dialog', { name: 'Delete "reports-2026"?' });
    await expect(confirm).toContainText('Folders are deleted with everything inside them');
    await confirm.getByRole('button', { name: 'Cancel' }).click();
    await expect(row(panel, `${HOME}/reports-2026`)).toBeVisible();
    await contextMenu(app, row(panel, `${HOME}/reports-2026`), 'Delete');
    await app.getByRole('dialog', { name: 'Delete "reports-2026"?' }).getByRole('button', { name: 'Delete' }).click();
    await expect(row(panel, `${HOME}/reports-2026`)).toHaveCount(0);

    // "Confirm before deleting" off: no question.
    await updateSettings(app, { files: { confirmDelete: false } });
    await row(panel, `${HOME}/notes.txt`).click();
    await app.keyboard.press('Delete');
    await expect(row(panel, `${HOME}/notes.txt`)).toHaveCount(0);
    await expect(app.getByRole('dialog')).toHaveCount(0);
  });

  test('the permissions dialog changes a mode, by checkbox and in octal', async ({ app }) => {
    const script = row(panel, `${HOME}/deploy.sh`);
    await expect(script.locator('.sftp__col--permissions')).toHaveText('-rwxr-xr-x');
    await contextMenu(app, script, 'Permissions…');
    const dialog = app.getByRole('dialog', { name: 'Permissions — deploy.sh' });
    const octal = dialog.getByRole('textbox');
    await expect(octal).toHaveValue('0755');
    await octal.fill('0700');
    await expect(dialog.locator('.chmod__row').nth(1).getByRole('checkbox').first()).not.toBeChecked();
    // Group read back on with its checkbox: 0740.
    await dialog.locator('.chmod__row').nth(1).getByRole('checkbox').first().check();
    await expect(octal).toHaveValue('0740');
    await dialog.getByRole('button', { name: 'Apply' }).click();
    await expect(script.locator('.sftp__col--permissions')).toHaveText('-rwxr-----');

    await contextMenu(app, script, 'Permissions…');
    await app.getByRole('dialog', { name: 'Permissions — deploy.sh' }).getByRole('button', { name: 'Cancel' }).click();
    await expect(script.locator('.sftp__col--permissions')).toHaveText('-rwxr-----');
  });

  test('download: the transfer runs, finishes, and "Clear finished" removes it', async () => {
    await row(panel, `${HOME}/notes.txt`).click();
    await panel.getByRole('button', { name: 'Download selected' }).click();
    const transfer = panel.locator('.sftp__transfer', { hasText: 'notes.txt' });
    await expect(transfer).toHaveClass(/sftp__transfer--done/);
    await expect(transfer).toContainText('2');                 // its size, 2 KB
    await panel.getByRole('button', { name: 'Clear finished' }).click();
    await expect(panel.locator('.sftp__transfers')).toHaveCount(0);
  });

  test('upload: the toolbar button, and "Upload file as…" with a new name', async ({ app, os }) => {
    // Without a list of files the host asks for them (the mock picks a document).
    await panel.getByRole('button', { name: 'Upload files' }).click();
    await expect(row(panel, `${HOME}/quarterly-report.xlsx`)).toBeVisible();

    await mockDialog(app, 'openFile', localNotes(os));
    await contextMenu(app, listArea(panel), 'Upload file as…');
    const dialog = app.getByRole('dialog', { name: 'Upload "notes.txt" as' });
    await expect(dialog.getByRole('textbox')).toHaveValue('notes.txt');
    await dialog.getByRole('textbox').fill('notes-from-laptop.txt');
    await dialog.getByRole('button', { name: 'Upload' }).click();
    await expect(row(panel, `${HOME}/notes-from-laptop.txt`)).toBeVisible();

    // Cancelled picker: nothing happens.
    await contextMenu(app, listArea(panel), 'Upload file as…');
    await expect(app.getByRole('dialog')).toHaveCount(0);
  });

  test('an upload onto an existing name asks: Skip leaves it, Rename… keeps both', async ({ app, os }) => {
    await mockDialog(app, 'openFile', localNotes(os));
    await contextMenu(app, listArea(panel), 'Upload file as…');
    await app.getByRole('dialog', { name: 'Upload "notes.txt" as' }).getByRole('button', { name: 'Upload' }).click();
    const conflict = app.getByRole('dialog', { name: '"notes.txt" already exists' });
    await expect(conflict).toBeVisible();
    await conflict.getByRole('button', { name: 'Skip' }).click();
    await expect(conflict).toBeHidden();
    await expect(panel.locator('.sftp__transfer', { hasText: 'notes.txt' })).toHaveClass(/sftp__transfer--done/);

    await mockDialog(app, 'openFile', localNotes(os));
    await contextMenu(app, listArea(panel), 'Upload file as…');
    await app.getByRole('dialog', { name: 'Upload "notes.txt" as' }).getByRole('button', { name: 'Upload' }).click();
    await app.getByRole('dialog', { name: '"notes.txt" already exists' }).getByRole('button', { name: 'Rename…' }).click();
    const again = app.getByRole('dialog', { name: '"notes.txt" already exists' });
    await again.getByRole('textbox').fill('notes (laptop).txt');
    await again.getByRole('button', { name: 'Save as this name' }).click();
    await expect(row(panel, `${HOME}/notes (laptop).txt`)).toBeVisible();
    await expect(row(panel, `${HOME}/notes.txt`)).toBeVisible();
  });

  test('"Download as…" takes the name from the Save dialog', async ({ app, os }) => {
    const target = os === 'darwin' ? '/Users/demo/Desktop/report-copy.pdf' : 'C:\\Users\\demo\\Desktop\\report-copy.pdf';
    await mockDialog(app, 'saveFile', target);
    await contextMenu(app, row(panel, `${HOME}/report.pdf`), 'Download as…');
    await expect(panel.locator('.sftp__transfer', { hasText: 'report-copy.pdf' })).toHaveClass(/sftp__transfer--done/);
    // Folders can't be downloaded "as" (only files).
    await row(panel, `${HOME}/logs`).click({ button: 'right' });
    await expect(app.getByRole('menuitem', { name: 'Download as…' })).toBeDisabled();
  });

  test('context menus: Copy path, Copy current path, Refresh', async ({ app }) => {
    const clipboard = () => app.evaluate(() => (globalThis as { __mockClipboard?: string }).__mockClipboard);
    await contextMenu(app, row(panel, `${HOME}/report.pdf`), 'Copy path');
    await expect.poll(clipboard).toBe(`${HOME}/report.pdf`);
    await row(panel, `${HOME}/logs`).dblclick();
    await expect(pathBox(app)).toHaveValue(`${HOME}/logs`);
    await contextMenu(app, listArea(panel), 'Copy current path');
    await expect.poll(clipboard).toBe(`${HOME}/logs`);
    await contextMenu(app, listArea(panel), /^Refresh/);
    await expect(pathBox(app)).toHaveValue(`${HOME}/logs`);
  });

  test('"Open in the file manager" opens the two-pane file manager for the same server', async ({ app }) => {
    await panel.getByRole('button', { name: 'Open in the file manager' }).click();
    await expect(app.getByRole('tab', { name: /web-01/, selected: true })).toBeVisible();
    await expect(app.getByRole('grid')).toHaveCount(2);
  });

  test('the panel closes with its button', async ({ app }) => {
    await panel.getByRole('button', { name: 'Close the SFTP panel' }).click();
    await expect(pathBox(app)).toHaveCount(0);
  });
});

test('a Telnet session has no SFTP panel button', async ({ app }) => {
  await sessionItem(app, 'router').dblclick();
  await expect(app.getByRole('tab', { name: /router/ })).toBeVisible();
  await expect(app.getByRole('contentinfo').getByRole('button', { name: 'SFTP', exact: true })).toHaveCount(0);
});
