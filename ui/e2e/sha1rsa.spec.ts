/**
 * Old servers that only speak SHA-1 RSA (`ssh-rsa`, OpenSSH before 7.2; 1.2.0 item 2). The mock's OLD_RSA_HOST
 * (src/bridge/mock/conn.ts) fails like the native side does unless "Allow old SHA-1 RSA" is on: the banner says what to
 * turn on, and its "Algorithm settings" button opens the session editor there.
 */
import { expect, test } from './fixtures';
import { editSession, editorPage, sessionItem, terminalText, useDomRenderer } from './helpers';
import type { Locator, Page } from '@playwright/test';

const HOST = 'old-rsa.example.test';

async function quickConnect(app: Page, target: string) {
  await app.getByRole('button', { name: 'New tab options' }).click();
  await app.getByRole('menuitem', { name: /Quick connect/ }).click();
  const dialog = app.getByRole('dialog', { name: 'Quick connect' });
  await dialog.getByRole('textbox').fill(target);
  await dialog.getByRole('button', { name: 'Connect' }).click();
}

const sha1Row = (dialog: Locator) =>
  dialog.locator('.dlg-row', { has: dialog.page().getByText('Allow old SHA-1 RSA (ssh-rsa), for servers older than OpenSSH 7.2', { exact: true }) });

test.describe('old servers that only speak ssh-rsa', () => {
  test('the failure says what to turn on; the switch, saved, lets the session connect', async ({ app }) => {
    await useDomRenderer(app);
    await quickConnect(app, `deploy@${HOST}`);
    const banner = app.locator('.term-banner');
    await expect(banner).toContainText('Connection failed');
    await expect(banner).toContainText('This server only supports the old SHA-1 RSA signatures (ssh-rsa).');
    await banner.getByRole('button', { name: 'Algorithm settings' }).click();

    // A quick-connect session is not saved: the editor opens a copy of it, on its Algorithms page.
    const editor = app.getByRole('dialog', { name: /session/i });
    await expect(editor.getByRole('heading', { level: 2, name: 'Algorithms' })).toBeVisible();
    await expect(sha1Row(editor).getByRole('switch')).not.toBeChecked();
    await expect(editor.getByText('Weak algorithms enabled')).toHaveCount(0);
    await sha1Row(editor).locator('label.switch').click();
    await expect(sha1Row(editor).getByRole('switch')).toBeChecked();
    await expect(editor.getByText('Weak algorithms enabled')).toBeVisible();  // it is a weak algorithm
    await editorPage(editor, 'Session');
    await editor.getByLabel('Name', { exact: true }).fill('old box');
    await editor.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(editor).toBeHidden();

    // The failed quick-connect tab closed, the saved session connects.
    await banner.getByRole('button', { name: 'Close' }).click();
    await expect(banner).toHaveCount(0);
    await sessionItem(app, 'old box').dblclick();
    await expect(terminalText(app)).toContainText('$');
    await expect(app.locator('.term-banner')).toHaveCount(0);
    const again = await editSession(app, 'old box');
    await editorPage(again, 'Algorithms');
    await expect(sha1Row(again).getByRole('switch')).toBeChecked();
  });

  test('other sessions: the switch is off and nothing is weak', async ({ app }) => {
    const editor = await editSession(app, 'web-01');
    await editorPage(editor, 'Algorithms');
    await expect(sha1Row(editor).getByRole('switch')).not.toBeChecked();
    await expect(editor.getByText('Weak algorithms enabled')).toHaveCount(0);
  });
});
