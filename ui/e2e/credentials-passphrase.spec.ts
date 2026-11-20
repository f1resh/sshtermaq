import { expect, test } from './fixtures';
import { confirmClose, editSession, editorPage, sessionItem, terminalText, useDomRenderer } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * Credentials, step 3 (remembered passphrases) against the mock backend: the linked key "router" is passphrase
 * protected and starts with its passphrase remembered. A mock connection whose Credentials key is protected and has
 * none remembered asks for it (src/bridge/mock/conn.ts; any passphrase but "wrong" opens it).
 */
async function openCredentials(page: Page): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Credentials', exact: true }).click();
  await expect(dialog.getByRole('heading', { level: 2, name: 'Credentials' })).toBeVisible();
  return dialog;
}
const row = (dialog: Locator, name: string) => dialog.locator('.vault-row', { has: dialog.page().locator('.vault-name', { hasText: new RegExp(`^\\s*${name}$`) }) });

test.describe('Credentials: remembered passphrases', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
  });

  test('forget a passphrase on the page; the prompt at connect remembers it again', async ({ app }) => {
    let dialog = await openCredentials(app);
    await expect(row(dialog, 'router')).toContainText('Passphrase remembered');
    await expect(row(dialog, 'work-laptop').getByRole('button', { name: 'Forget passphrase' })).toHaveCount(0);  // none to forget
    await row(dialog, 'router').getByRole('button', { name: 'Forget passphrase' }).click();
    await expect(row(dialog, 'router')).not.toContainText('Passphrase remembered');
    await expect(row(dialog, 'router')).toContainText('Passphrase');
    await expect(row(dialog, 'router').getByRole('button', { name: 'Forget passphrase' })).toHaveCount(0);
    await dialog.getByRole('button', { name: 'Close' }).first().click();

    // web-01 signs in with "router": connecting asks for its passphrase, with "Remember for this key".
    const editor = await editSession(app, 'web-01');
    await editorPage(editor, 'Authentication');
    await editor.getByRole('radiogroup', { name: 'Key source' }).getByRole('radio', { name: 'From Credentials' }).click();
    await editor.getByRole('combobox', { name: 'Key from Credentials' }).selectOption({ label: 'router · rsa' });
    await editor.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(editor).toBeHidden();

    await sessionItem(app, 'web-01').dblclick();
    const card = app.locator('.prompt-card');
    await expect(card).toContainText('Passphrase for key');
    await expect(card).toContainText('router');
    const remember = card.getByRole('checkbox', { name: 'Remember for this key' });
    await expect(remember).not.toBeChecked();
    await card.locator('input[type="password"]').fill('wrong');
    await card.getByRole('button', { name: 'Log in' }).click();
    await expect(card).toContainText('Wrong passphrase, try again.');
    await card.locator('input[type="password"]').fill('demo');
    await remember.check();
    await card.getByRole('button', { name: 'Log in' }).click();
    await expect(card).toHaveCount(0);
    await expect(terminalText(app)).toContainText('deploy@web-01');

    dialog = await openCredentials(app);
    await expect(row(dialog, 'router')).toContainText('Passphrase remembered');
    // Deleting the key says that its passphrase goes too.
    await row(dialog, 'router').getByRole('button', { name: 'Delete' }).click();
    await expect(app.getByRole('dialog', { name: 'Delete "router"?' })).toContainText('Its remembered passphrase is forgotten too.');
  });

  test('without "Remember for this key" the next connection asks again', async ({ app }) => {
    const dialog = await openCredentials(app);
    await row(dialog, 'router').getByRole('button', { name: 'Forget passphrase' }).click();
    await expect(row(dialog, 'router').getByRole('button', { name: 'Forget passphrase' })).toHaveCount(0);
    await dialog.getByRole('button', { name: 'Close' }).first().click();
    const editor = await editSession(app, 'db-primary');
    await editorPage(editor, 'Authentication');
    await editor.getByRole('radiogroup', { name: 'Key source' }).getByRole('radio', { name: 'From Credentials' }).click();
    await editor.getByRole('combobox', { name: 'Key from Credentials' }).selectOption({ label: 'router · rsa' });
    await editor.getByRole('button', { name: 'Save', exact: true }).click();

    const card = app.locator('.prompt-card');
    for (let i = 0; i < 2; i++) {
      await sessionItem(app, 'db-primary').dblclick();
      await card.locator('input[type="password"]').fill('demo');
      await card.getByRole('button', { name: 'Log in' }).click();
      await expect(card).toHaveCount(0);
      await expect(terminalText(app)).toContainText('postgres@db-primary');
      const tab = app.getByRole('tab', { name: /db-primary/ });
      await tab.getByRole('button', { name: 'Close tab' }).click();
      await confirmClose(app);
      await expect(tab).toHaveCount(0);
    }
  });
});
