import { expect, test } from './fixtures';
import { editSession, editorPage, mockDialog, openPalette } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * Credentials, step 1 (keys) against the mock backend (src/bridge/mock/keys.ts): it starts with a stored key
 * "work-laptop" (ed25519) and a linked file "router" (ppk3, passphrase protected).
 */
const PPK = [
  'PuTTY-User-Key-File-3: ssh-ed25519',
  'Encryption: none',
  'Comment: deploy@ci',
  'Public-Lines: 2',
  'AAAAC3NzaC1lZDI1NTE5AAAAIMockPublic',
  'AAAA',
  'Private-Lines: 1',
  'AAAAIMockPrivate',
  'Private-MAC: 0000'
].join('\n');

async function openCredentials(page: Page): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Credentials', exact: true }).click();
  await expect(dialog.getByRole('heading', { level: 2, name: 'Credentials' })).toBeVisible();
  return dialog;
}
const row = (dialog: Locator, name: string) => dialog.locator('.vault-row', { has: dialog.page().locator('.vault-name', { hasText: new RegExp(`^\\s*${name}$`) }) });

test.describe('Credentials: keys', () => {
  test('the page lists stored and linked keys; copy, rename, delete', async ({ app }) => {
    const dialog = await openCredentials(app);
    await expect(row(dialog, 'work-laptop')).toContainText('Stored');
    await expect(row(dialog, 'work-laptop')).toContainText('ed25519');
    await expect(row(dialog, 'router')).toContainText('router.ppk');
    await expect(row(dialog, 'router')).toContainText('Passphrase');
    await expect(row(dialog, 'router')).toContainText('unused');

    await row(dialog, 'work-laptop').getByRole('button', { name: 'Copy public key' }).click();
    await expect.poll(() => app.evaluate(() => (globalThis as { __mockClipboard?: string }).__mockClipboard ?? '')).toMatch(/^ssh-ed25519 AAAA/);

    await row(dialog, 'router').getByRole('button', { name: 'Rename' }).click();
    const rename = app.getByRole('dialog', { name: 'Rename "router"' });
    await rename.getByRole('textbox', { name: 'Key name' }).fill('WORK-LAPTOP');
    await expect(rename).toContainText('already exists');
    await expect(rename.getByRole('button', { name: 'Rename' })).toBeDisabled();
    await rename.getByRole('textbox', { name: 'Key name' }).fill('home router');
    await rename.getByRole('button', { name: 'Rename' }).click();
    await expect(row(dialog, 'home router')).toBeVisible();

    await row(dialog, 'home router').getByRole('button', { name: 'Delete' }).click();
    const del = app.getByRole('dialog', { name: 'Delete "home router"?' });
    await expect(del).toContainText('Only the link is removed');
    await del.getByRole('button', { name: 'Delete' }).click();
    await expect(row(dialog, 'home router')).toHaveCount(0);
  });

  test('pasting: junk and public keys are refused; a private key is shown, named from its comment, and added', async ({ app }) => {
    const dialog = await openCredentials(app);
    await dialog.getByRole('button', { name: 'Paste a key…' }).click();
    const paste = app.getByRole('dialog', { name: 'Paste a private key' });
    const text = paste.getByRole('textbox', { name: 'Private key text' });
    const add = paste.getByRole('button', { name: 'Add to Credentials' });

    await text.fill('hello world');
    await expect(paste).toContainText('Not a recognised private key file');
    await expect(add).toBeDisabled();
    await text.fill('ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMockPublic me@laptop');
    await expect(paste).toContainText('This is a public key: paste the private key');

    await text.fill(PPK);
    await expect(paste).toContainText('PuTTY v3');
    await expect(paste).toContainText('No passphrase');
    await expect(paste.getByRole('textbox', { name: 'Key name' })).toHaveValue('deploy@ci');
    await add.click();
    await expect(paste).toBeHidden();
    await expect(row(dialog, 'deploy@ci')).toContainText('Stored');
  });

  test('linking a key file keeps only its path', async ({ app, os }) => {
    const dialog = await openCredentials(app);
    const path = os === 'darwin' ? '/Users/demo/.ssh/id_ecdsa' : 'C:\\Users\\demo\\.ssh\\id_ecdsa';
    await mockDialog(app, 'openFile', path);
    await dialog.getByRole('button', { name: 'Link a key file…' }).click();
    const link = app.getByRole('dialog', { name: 'Add a key file to Credentials' });
    await expect(link).toContainText('Only the path is stored');
    await expect(link.getByRole('textbox', { name: 'Key name' })).toHaveValue('id_ecdsa@workstation');
    await link.getByRole('textbox', { name: 'Key name' }).fill('ci ecdsa');
    await link.getByRole('button', { name: 'Add to Credentials' }).click();
    await expect(row(dialog, 'ci ecdsa')).toContainText('id_ecdsa');
    await expect(row(dialog, 'ci ecdsa').locator('.vault-where')).toHaveAttribute('title', path);
  });

  test('a session uses a key from Credentials; the page counts it and the delete question names it', async ({ app }) => {
    const editor = await editSession(app, 'web-01');
    await editorPage(editor, 'Authentication');
    await editor.getByRole('radiogroup', { name: 'Key source' }).getByRole('radio', { name: 'From Credentials' }).click();
    await editor.getByRole('combobox', { name: 'Key from Credentials' }).selectOption({ label: 'work-laptop · ed25519' });
    await expect(editor).toContainText('Stored in Credentials');
    await editor.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(editor).toBeHidden();

    // Opened again: still chosen.
    const again = await editSession(app, 'web-01');
    await editorPage(again, 'Authentication');
    await expect(again.getByRole('combobox', { name: 'Key from Credentials' })).toHaveValue('vault-work-laptop');
    await again.getByRole('button', { name: 'Cancel' }).click();

    const dialog = await openCredentials(app);
    await expect(row(dialog, 'work-laptop')).toContainText('1 session');
    await row(dialog, 'work-laptop').getByRole('button', { name: 'Delete' }).click();
    const del = app.getByRole('dialog', { name: 'Delete "work-laptop"?' });
    await expect(del).toContainText('Used by 1 session and 1 login');  // the seeded login deploy@prod uses it too
    await expect(del).toContainText('web-01, login deploy@prod');
    await del.getByRole('button', { name: 'Cancel' }).click();
  });

  test('"Paste key…" in the session editor stores the key under its name and selects it', async ({ app }) => {
    const editor = await editSession(app, 'web-02');
    await editorPage(editor, 'Authentication');
    await editor.getByRole('button', { name: 'Paste key…' }).click();
    const paste = app.getByRole('dialog', { name: 'Paste a private key' });
    await paste.getByRole('textbox', { name: 'Private key text' }).fill(PPK);
    await expect(paste.getByRole('textbox', { name: 'Key name' })).toHaveValue('deploy@ci');
    await paste.getByRole('textbox', { name: 'Key name' }).fill('ci deploy');
    await paste.getByRole('button', { name: 'Add to Credentials' }).click();
    await expect(paste).toBeHidden();
    await expect(editor.getByRole('radiogroup', { name: 'Key source' }).getByRole('radio', { name: 'From Credentials' })).toBeChecked();
    await expect(editor.getByRole('combobox', { name: 'Key from Credentials' }).locator('option:checked')).toHaveText('ci deploy · ed25519');
  });

  test('the key generator saves a new key to Credentials', async ({ app, os }) => {
    const palette = await openPalette(app, os);
    await app.keyboard.type('key generator');
    await palette.getByRole('option', { name: /Key generator/ }).click();
    const keygen = app.getByRole('dialog', { name: 'Key generator' });
    await keygen.getByRole('button', { name: 'Generate', exact: true }).click();
    await keygen.getByRole('button', { name: 'Save to Credentials…' }).click();
    const save = app.getByRole('dialog', { name: 'Paste a private key' });
    await expect(save.getByRole('textbox', { name: 'Private key text' })).toHaveValue(/BEGIN OPENSSH PRIVATE KEY/);
    await save.getByRole('textbox', { name: 'Key name' }).fill('fresh key');
    await save.getByRole('button', { name: 'Add to Credentials' }).click();
    await expect(app.getByRole('status')).toContainText('Saved to Credentials');
    await keygen.getByRole('button', { name: 'Close' }).last().click();
    const dialog = await openCredentials(app);
    await expect(row(dialog, 'fresh key')).toContainText('Stored');
  });
});
