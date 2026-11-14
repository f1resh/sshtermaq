import { expect, test } from './fixtures';
import { sessionItem } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * The import wizard against the mock importer (src/bridge/mock/importer.ts): its 8 PuTTY sessions include raspberrypi
 * and router, which the mock's saved sessions already have at the top level, plus build-server, legacy-switch (with
 * notes) and others; its ~/.ssh/config has github.com, bastion, app-* (with a note) and staging.
 */
const puttyTitle = (os: 'win32' | 'darwin') => `Import from PuTTY (${os === 'darwin' ? '~/.putty' : 'this PC'})`;

/** The sidebar's Import button: goes straight to the preview of PuTTY's sessions. */
async function openPuttyPreview(page: Page, os: 'win32' | 'darwin'): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Import', exact: true }).click();
  const preview = page.getByRole('dialog', { name: puttyTitle(os) });
  await expect(preview.locator('.imp-item', { hasText: 'build-server' })).toBeVisible();
  return preview;
}

/** The list of sources: one step back from the PuTTY preview. */
async function openSources(page: Page, os: 'win32' | 'darwin'): Promise<Locator> {
  const preview = await openPuttyPreview(page, os);
  await preview.getByRole('button', { name: 'Back' }).click();
  const dialog = page.getByRole('dialog', { name: 'Import sessions' });
  await expect(dialog).toBeVisible();
  return dialog;
}

const item = (preview: Locator, name: string) => preview.locator('.imp-item', { hasText: name });
const counter = (preview: Locator) => preview.getByText(/^\d+ of \d+ selected$/);

test.describe('import', () => {
  test('offers the sources of this system and counts the PuTTY sessions', async ({ app, os }) => {
    const dialog = await openSources(app, os);
    const putty = dialog.getByRole('button', { name: new RegExp(`^PuTTY on ${os === 'darwin' ? 'this Mac' : 'this PC'}`) });
    await expect(putty).toContainText('8 sessions');
    await expect(putty).toContainText(os === 'darwin' ? '~/.putty' : 'registry');
    await expect(dialog.getByRole('button', { name: /^PuTTY \.reg file/ })).toBeVisible();
    await expect(dialog.getByRole('button', { name: /^OpenSSH config/ })).toBeVisible();
    await expect(dialog.getByRole('button', { name: /^SSHTermaq backup/ })).toBeVisible();
    await dialog.getByRole('button', { name: 'Cancel' }).click();
    await expect(dialog).toBeHidden();
  });

  test('PuTTY: notes, filter, select, import into the PuTTY folder', async ({ app, os }) => {
    const preview = await openPuttyPreview(app, os);
    await expect(preview).toContainText('invalid port number');                   // a note about a skipped entry
    await item(preview, 'legacy-switch').getByTitle('Show import notes').click();
    await expect(preview).toContainText('SSH-1 is not supported');

    // The filter narrows the list.
    await preview.getByPlaceholder('Filter sessions').fill('legacy');
    await expect(preview.locator('.imp-item')).toHaveCount(1);
    await preview.getByPlaceholder('Filter sessions').fill('');
    await expect(counter(preview)).toHaveText('8 of 8 selected');

    // "Select all" off, then pick two.
    await preview.getByRole('checkbox').first().uncheck();
    await expect(counter(preview)).toHaveText('0 of 8 selected');
    await item(preview, 'build-server').getByRole('checkbox').check();
    await item(preview, 'legacy-switch').getByRole('checkbox').check();
    await expect(counter(preview)).toHaveText('2 of 8 selected');
    await expect(preview.getByPlaceholder('(top level)')).toHaveValue('PuTTY');
    await preview.getByRole('button', { name: 'Import 2 sessions' }).click();

    const done = app.getByRole('dialog', { name: 'Import complete' });
    await expect(done.locator('.imp-stat', { hasText: 'Imported' }).locator('.imp-stat__n')).toHaveText('2');
    await expect(done.locator('.imp-stat', { hasText: 'Host keys' }).locator('.imp-stat__n')).toHaveText('4');
    await done.getByRole('button', { name: 'Done' }).click();
    await expect(app.getByRole('tree', { name: 'Sessions' }).getByRole('treeitem', { name: /^PuTTY/ })).toBeVisible();
    await expect(sessionItem(app, 'build-server')).toBeVisible();
    await expect(sessionItem(app, 'legacy-switch')).toBeVisible();
  });

  test('existing sessions at the top level: marked, and replaced when chosen', async ({ app, os }) => {
    const preview = await openPuttyPreview(app, os);
    await preview.getByPlaceholder('(top level)').fill('');
    await expect(item(preview, 'raspberrypi').getByText('exists', { exact: true })).toBeVisible();
    await expect(item(preview, 'router').getByText('exists', { exact: true })).toBeVisible();
    await expect(item(preview, 'build-server').getByText('exists', { exact: true })).toHaveCount(0);

    await preview.getByRole('checkbox').first().uncheck();
    await item(preview, 'raspberrypi').getByRole('checkbox').check();
    await expect(preview).toContainText('1 of the selected sessions already exist');
    await preview.locator('select').filter({ has: app.locator('option', { hasText: 'Replace the saved session' }) }).selectOption({ label: 'Replace the saved session' });
    await preview.getByRole('button', { name: 'Import 1 session' }).click();

    const done = app.getByRole('dialog', { name: 'Import complete' });
    await expect(done.locator('.imp-stat', { hasText: 'Replaced' }).locator('.imp-stat__n')).toHaveText('1');
    await done.getByRole('button', { name: 'Done' }).click();
    await expect(sessionItem(app, 'raspberrypi')).toHaveCount(1);
  });

  test('OpenSSH config into a folder of its own, with the notes of a host', async ({ app, os }) => {
    const dialog = await openSources(app, os);
    await dialog.getByRole('button', { name: /^OpenSSH config/ }).click();
    const preview = app.getByRole('dialog', { name: /^Import from / });
    await expect(item(preview, 'app-*')).toBeVisible();
    await item(preview, 'app-*').getByTitle('Show import notes').click();
    await expect(preview).toContainText('contains wildcards');

    await preview.getByPlaceholder('(top level)').fill('From ssh config');
    await preview.getByRole('button', { name: /^Import \d+ sessions?$/ }).click();
    const done = app.getByRole('dialog', { name: 'Import complete' });
    await expect(done.locator('.imp-stat', { hasText: 'Imported' }).locator('.imp-stat__n')).not.toHaveText('0');
    // "Import more" goes back to the sources.
    await done.getByRole('button', { name: 'Import more' }).click();
    await expect(app.getByRole('dialog', { name: 'Import sessions' })).toBeVisible();
    await app.keyboard.press('Escape');
    await expect(app.getByRole('tree', { name: 'Sessions' }).getByRole('treeitem', { name: /^From ssh config/ })).toBeVisible();
    await expect(sessionItem(app, 'github.com')).toBeVisible();
  });

  test('a .reg file and a JSON backup show their previews', async ({ app, os }) => {
    let dialog = await openSources(app, os);
    await dialog.getByRole('button', { name: /^PuTTY \.reg file/ }).click();
    let preview = app.getByRole('dialog', { name: /^Import from / });
    await expect(item(preview, 'build-server')).toBeVisible();
    await preview.getByRole('button', { name: 'Back' }).click();

    dialog = app.getByRole('dialog', { name: 'Import sessions' });
    await dialog.getByRole('button', { name: /^SSHTermaq backup/ }).click();
    preview = app.getByRole('dialog', { name: /^Import from / });
    await expect(preview.locator('.imp-item').first()).toBeVisible();
    await expect(counter(preview)).toBeVisible();
  });

  test('a session exports to a PuTTY .reg file and to JSON', async ({ app }) => {
    await sessionItem(app, 'web-01').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Export to PuTTY .reg…' }).click();
    const toasts = app.getByRole('status');
    await expect(toasts).toContainText('Exported');
    await expect(toasts).toContainText('.reg');
    await sessionItem(app, 'web-01').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Export JSON…' }).click();
    await expect(toasts).toContainText('.json');
  });
});
