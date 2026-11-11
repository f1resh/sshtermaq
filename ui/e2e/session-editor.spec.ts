import { expect, test } from './fixtures';
import { editorPage, editSession, sessionItem } from './helpers';

test.describe('session editor', () => {
  test('shows the session and its categories', async ({ app }) => {
    const dialog = await editSession(app, 'web-01');
    await expect(dialog.getByRole('textbox', { name: 'Name', exact: true })).toHaveValue('web-01');
    await expect(dialog.getByRole('combobox', { name: 'Folder' })).toHaveValue('Production/Web');
    for (const category of ['SSH', 'Authentication', 'Tunnels', 'Proxy', 'Terminal', 'Logging']) await editorPage(dialog, category);
  });

  test('saved password: says what encrypts it on this system', async ({ app, os }) => {
    const dialog = await editSession(app, 'web-01');
    await editorPage(dialog, 'Authentication');
    const text = dialog.getByText(/^Encrypted with .* and used automatically/);
    await expect(text).toBeVisible();
    if (os === 'darwin') {
      await expect(text).toContainText('login Keychain');
      await expect(dialog.getByText(/DPAPI/)).toHaveCount(0);
    } else {
      await expect(text).toContainText('Windows DPAPI');
      await expect(dialog.getByText(/Keychain/)).toHaveCount(0);
    }
  });

  test('proxy password: says what encrypts it on this system', async ({ app, os }) => {
    const dialog = await editSession(app, 'web-01');
    await editorPage(dialog, 'Proxy');
    await dialog.getByRole('combobox').first().selectOption({ label: 'SOCKS 5' });
    await expect(dialog.getByText(os === 'darwin' ? 'Stored encrypted (key in your login Keychain)' : 'Stored encrypted (Windows DPAPI)')).toBeVisible();
  });

  test('renaming and saving updates the sidebar', async ({ app }) => {
    const dialog = await editSession(app, 'web-01');
    await dialog.getByRole('textbox', { name: 'Name', exact: true }).fill('web-01-renamed');
    await dialog.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(dialog).toBeHidden();
    await expect(sessionItem(app, 'web-01-renamed')).toBeVisible();
  });

  test('cancel asks before discarding changes, then keeps the session as it was', async ({ app }) => {
    const dialog = await editSession(app, 'web-01');
    await dialog.getByRole('textbox', { name: 'Name', exact: true }).fill('changed-name');
    await expect(dialog).toHaveAccessibleName('Edit session changed-name');
    await dialog.getByRole('button', { name: 'Cancel' }).click();
    const discard = app.getByRole('dialog', { name: 'Discard changes?' });
    await discard.getByRole('button', { name: 'Keep editing' }).click();
    await expect(dialog.getByRole('textbox', { name: 'Name', exact: true })).toHaveValue('changed-name');
    await dialog.getByRole('button', { name: 'Cancel' }).click();
    await discard.getByRole('button', { name: 'Discard changes' }).click();
    await expect(dialog).toBeHidden();
    await expect(sessionItem(app, 'web-01')).toBeVisible();
    await expect(sessionItem(app, 'changed-name')).toHaveCount(0);
  });
});
