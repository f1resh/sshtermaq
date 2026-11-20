import { expect, test } from './fixtures';
import { mockDialog, savedFiles } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * Credentials, step 5 (export / import) against the mock backend: it starts with the keys "work-laptop" (stored) and
 * "router" (linked, passphrase remembered) and the login "deploy@prod" (stored password). The mock's file is what its
 * save dialog wrote (src/bridge/mock/keys.ts); import reads it back by path.
 */
async function openCredentials(page: Page): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Credentials', exact: true }).click();
  await expect(dialog.getByRole('heading', { level: 2, name: 'Credentials' })).toBeVisible();
  return dialog;
}
const loginRow = (dialog: Locator, name: string) =>
  dialog.locator('.vault-login-row', { has: dialog.page().locator('.vault-name', { hasText: new RegExp(`^\\s*${name}$`) }) });
const FILE = 'C:/backup/credentials.sshtcred';

async function exportTo(page: Page, dialog: Locator, file: string | null, options: { secrets?: boolean } = {}): Promise<Locator> {
  await dialog.getByRole('button', { name: 'Export Credentials…' }).click();
  const ex = page.getByRole('dialog', { name: 'Export Credentials' });
  await ex.getByLabel('File password').fill('backup pass');
  await ex.getByLabel('Repeat it').fill('backup pass');
  if (options.secrets === false) await ex.getByRole('checkbox', { name: 'Include stored passwords and remembered passphrases' }).uncheck();
  await mockDialog(page, 'saveTextFile', file);
  await ex.getByRole('button', { name: 'Export…' }).click();
  return ex;
}

async function deleteLogin(page: Page, dialog: Locator, name: string): Promise<void> {
  await loginRow(dialog, name).getByRole('button', { name: 'Delete' }).click();
  await page.getByRole('dialog', { name: `Delete login "${name}"?` }).getByRole('button', { name: 'Delete' }).click();
  await expect(loginRow(dialog, name)).toHaveCount(0);
}

async function importFrom(page: Page, dialog: Locator, file: string, password: string): Promise<Locator> {
  await dialog.getByRole('button', { name: 'Import Credentials…' }).click();
  const im = page.getByRole('dialog', { name: 'Import Credentials' });
  await mockDialog(page, 'openFile', file);
  await im.getByRole('button', { name: 'Choose a file…' }).click();
  await expect(im).toContainText(file);
  await im.getByLabel('File password').fill(password);
  await im.getByRole('button', { name: 'Import' }).click();
  return im;
}

test.describe('Credentials: export and import', () => {
  test('export to a file sealed with a password; import it back (merged, wrong password refused)', async ({ app }) => {
    const dialog = await openCredentials(app);
    await dialog.getByRole('button', { name: 'Export Credentials…' }).click();
    const ex = app.getByRole('dialog', { name: 'Export Credentials' });
    await ex.getByLabel('File password').fill('short');
    await expect(ex).toContainText('Use at least 8 characters');
    await ex.getByLabel('File password').fill('backup pass');
    await ex.getByLabel('Repeat it').fill('backup pas');
    await expect(ex).toContainText('The two passwords differ');
    await expect(ex.getByRole('button', { name: 'Export…' })).toBeDisabled();
    await ex.getByRole('button', { name: 'Cancel' }).click();

    await exportTo(app, dialog, FILE);
    await expect(app.getByRole('status')).toContainText('2 keys and 1 login');
    const [file] = await savedFiles(app);
    expect(file.path).toBe(FILE);
    expect(file.secret).toBe(true);  // written owner-only

    // Gone here, back from the file; what is still here is left as it is.
    await deleteLogin(app, dialog, 'deploy@prod');
    let im = await importFrom(app, dialog, FILE, 'wrong pass');
    await expect(im).toContainText('Wrong password, or the file is damaged');
    await im.getByLabel('File password').fill('backup pass');
    await im.getByRole('button', { name: 'Import' }).click();
    await expect(im).toContainText('Added 1 login (1 secret).');
    await expect(im).toContainText('Already here, left as they are: 2 keys.');
    await im.getByRole('button', { name: 'Close' }).last().click();
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('password stored · key work-laptop');

    // Again: nothing new.
    im = await importFrom(app, dialog, FILE, 'backup pass');
    await expect(im).toContainText('Nothing new: everything in the file is already here.');
  });

  test('without its secrets a login asks for its password; a cancelled save exports nothing', async ({ app }) => {
    const dialog = await openCredentials(app);
    const cancelled = await exportTo(app, dialog, null);
    await expect(cancelled).toContainText('Not exported: no file was chosen.');
    await cancelled.getByRole('button', { name: 'Cancel' }).click();

    await exportTo(app, dialog, FILE, { secrets: false });
    await expect(app.getByRole('status')).toContainText('Credentials exported');
    await deleteLogin(app, dialog, 'deploy@prod');
    const im = await importFrom(app, dialog, FILE, 'backup pass');
    await expect(im).toContainText('Added 1 login.');
    await im.getByRole('button', { name: 'Close' }).last().click();
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('password asked');
  });

  test('locked Credentials are unlocked first', async ({ app }) => {
    await app.evaluate(async () => {
      const module = '/src/bridge/api.ts';
      const { api } = (await import(module)) as typeof import('../src/bridge/api');
      await api.vault.setMaster('correct horse');
      await api.vault.lock();
    });
    const dialog = await openCredentials(app);
    await exportTo(app, dialog, FILE);
    const unlock = app.getByRole('dialog', { name: 'Unlock Credentials' });
    await unlock.getByLabel('Master password').fill('correct horse');
    await unlock.getByRole('button', { name: 'Unlock' }).click();
    await expect(app.getByRole('status')).toContainText('Credentials exported');
    expect((await savedFiles(app)).length).toBe(1);
  });
});
