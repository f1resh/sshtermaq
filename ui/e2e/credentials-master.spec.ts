import { expect, test } from './fixtures';
import { openPalette, sessionItem, terminalText, useDomRenderer } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * Credentials, step 4 (the optional master password) against the mock backend: no master password at first; the
 * stored key "work-laptop" and the login "deploy@prod" (stored password) are its secrets. A mock connection that needs a
 * sealed secret while Credentials are locked asks "Unlock Credentials" (src/bridge/mock/conn.ts).
 */
async function openCredentials(page: Page): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Credentials', exact: true }).click();
  await expect(dialog.getByRole('heading', { level: 2, name: 'Credentials' })).toBeVisible();
  return dialog;
}
const PPK = [
  'PuTTY-User-Key-File-3: ssh-ed25519',
  'Encryption: none',
  'Comment: sealed@test',
  'Public-Lines: 2',
  'AAAAC3NzaC1lZDI1NTE5AAAAIMockPublic',
  'AAAA',
  'Private-Lines: 1',
  'AAAAIMockPrivate',
  'Private-MAC: 0000'
].join('\n');
const masterHeading = (dialog: Locator) => dialog.getByRole('heading', { level: 3, name: /^Master password/ });
const loginRow = (dialog: Locator, name: string) =>
  dialog.locator('.vault-login-row', { has: dialog.page().locator('.vault-name', { hasText: new RegExp(`^\\s*${name}$`) }) });

/** Calls the bridge the way the app does (the mock in the page). */
async function vaultCall(page: Page, method: 'setMaster' | 'lock', ...args: string[]): Promise<void> {
  await page.evaluate(
    async ([m, a]) => {
      const module = '/src/bridge/api.ts';
      const { api } = (await import(module)) as typeof import('../src/bridge/api');
      await (api.vault[m as 'setMaster'] as (...x: string[]) => Promise<void>)(...(a as string[]));
    },
    [method, args] as const
  );
}

test.describe('Credentials: master password', () => {
  test('set it, lock now, and a locked page asks to unlock before storing a secret', async ({ app }) => {
    const dialog = await openCredentials(app);
    await expect(masterHeading(dialog)).toContainText('Off');
    await dialog.getByRole('button', { name: 'Set a master password…' }).click();
    const set = app.getByRole('dialog', { name: 'Set a master password' });
    await expect(set).toContainText('It cannot be recovered');
    const submit = set.getByRole('button', { name: 'Set master password' });
    await set.getByLabel('Master password', { exact: true }).fill('short');
    await expect(set).toContainText('Use at least 8 characters');
    await set.getByLabel('Master password', { exact: true }).fill('correct horse');
    await set.getByLabel('Repeat it').fill('correct hors');
    await expect(set).toContainText('The two passwords differ');
    await expect(submit).toBeDisabled();
    await set.getByLabel('Repeat it').fill('correct horse');
    await submit.click();
    await expect(set).toBeHidden();
    await expect(masterHeading(dialog)).toContainText('Unlocked');

    await dialog.getByRole('button', { name: 'Lock now' }).click();
    await expect(masterHeading(dialog)).toContainText('Locked');
    await expect(dialog.getByText('Credentials are locked', { exact: true })).toBeVisible();

    // Pasting a key needs the master password: the unlock dialog comes first, then the key is added.
    await dialog.getByRole('button', { name: 'Paste a key…' }).click();
    const paste = app.getByRole('dialog', { name: 'Paste a private key' });
    await paste.getByRole('textbox', { name: 'Private key text' }).fill(PPK);
    await paste.getByRole('textbox', { name: 'Key name' }).fill('sealed key');
    await paste.getByRole('button', { name: 'Add to Credentials' }).click();
    const unlock = app.getByRole('dialog', { name: 'Unlock Credentials' });
    await unlock.getByLabel('Master password').fill('wrong password');
    await unlock.getByRole('button', { name: 'Unlock' }).click();
    await expect(unlock).toContainText('Wrong master password, try again.');
    await unlock.getByLabel('Master password').fill('correct horse');
    await unlock.getByRole('button', { name: 'Unlock' }).click();
    await expect(unlock).toBeHidden();
    await expect(paste).toBeHidden();
    await expect(dialog.locator('.vault-row', { hasText: 'sealed key' })).toBeVisible();
    await expect(masterHeading(dialog)).toContainText('Unlocked');
  });

  test('a connection that needs a sealed secret asks "Unlock Credentials" once; the palette locks again', async ({ app, os }) => {
    await useDomRenderer(app);
    await vaultCall(app, 'setMaster', 'correct horse');
    await vaultCall(app, 'lock');
    // web-01 signs in with the login deploy@prod (a stored password).
    await sessionItem(app, 'web-01').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Use login' }).hover();
    await app.getByRole('menuitem', { name: 'deploy@prod (deploy)' }).click();

    await sessionItem(app, 'web-01').dblclick();
    const card = app.locator('.prompt-card');
    await expect(card).toContainText('Unlock Credentials');
    await card.locator('input[type="password"]').fill('nope nope');
    await card.getByRole('button', { name: 'Unlock' }).click();
    await expect(card).toContainText('Wrong master password, try again.');
    await card.locator('input[type="password"]').fill('correct horse');
    await card.getByRole('button', { name: 'Unlock' }).click();
    await expect(card).toHaveCount(0);
    await expect(terminalText(app)).toContainText('deploy@web-01');

    const palette = await openPalette(app, os);
    await app.keyboard.type('lock credentials');
    await palette.getByRole('option', { name: /Lock Credentials/ }).click();
    await expect(app.getByRole('status')).toContainText('Credentials locked');
    const dialog = await openCredentials(app);
    await expect(masterHeading(dialog)).toContainText('Locked');
  });

  test('change, remove, and reset when it is forgotten', async ({ app }) => {
    await vaultCall(app, 'setMaster', 'correct horse');
    const dialog = await openCredentials(app);
    await expect(masterHeading(dialog)).toContainText('Unlocked');

    await dialog.getByRole('button', { name: 'Change…' }).click();
    const change = app.getByRole('dialog', { name: 'Change the master password' });
    await change.getByLabel('Current master password').fill('wrong one!');
    await change.getByLabel('New master password').fill('battery staple');
    await change.getByLabel('Repeat the new one').fill('battery staple');
    await change.getByRole('button', { name: 'Change' }).click();
    await expect(change).toContainText('The current master password is wrong');
    await change.getByLabel('Current master password').fill('correct horse');
    await change.getByRole('button', { name: 'Change' }).click();
    await expect(change).toBeHidden();

    await dialog.getByRole('button', { name: 'Remove…' }).click();
    const remove = app.getByRole('dialog', { name: 'Remove the master password' });
    await remove.getByLabel('Current master password').fill('battery staple');
    await remove.getByRole('button', { name: 'Remove' }).click();
    await expect(remove).toBeHidden();
    await expect(masterHeading(dialog)).toContainText('Off');

    // Forgotten: set again, lock, reset. The stored key and the login's password go; linked keys and logins stay.
    await vaultCall(app, 'setMaster', 'forgotten soon');
    await vaultCall(app, 'lock');
    await expect(masterHeading(dialog)).toContainText('Locked');
    await dialog.getByRole('button', { name: 'Forgot it: reset Credentials…' }).click();
    const reset = app.getByRole('dialog', { name: 'Reset Credentials?' });
    await expect(reset).toContainText('1 stored key');
    await expect(reset).toContainText('1 login password');
    await expect(reset).toContainText('1 remembered passphrase');
    await reset.getByRole('button', { name: 'Delete the stored secrets' }).click();
    await expect(reset).toBeHidden();
    await expect(masterHeading(dialog)).toContainText('Off');
    await expect(dialog.locator('.vault-row', { hasText: 'work-laptop' })).toHaveCount(0);
    await expect(dialog.locator('.vault-row', { hasText: 'router' })).toBeVisible();
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('password asked');
  });
});
