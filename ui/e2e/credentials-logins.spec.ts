import { expect, test } from './fixtures';
import { editSession, editorPage, sessionItem } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * Credentials, step 2 (logins) against the mock backend (src/bridge/mock): it starts with the login "deploy@prod"
 * (user deploy, stored password, the stored key "work-laptop").
 */
async function openCredentials(page: Page): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Credentials', exact: true }).click();
  await expect(dialog.getByRole('heading', { level: 2, name: 'Credentials' })).toBeVisible();
  return dialog;
}
const loginRow = (dialog: Locator, name: string) =>
  dialog.locator('.vault-login-row', { has: dialog.page().locator('.vault-name', { hasText: new RegExp(`^\\s*${name.replace(/[()]/g, '\\$&')}$`) }) });

test.describe('Credentials: logins', () => {
  test('the page lists logins with how they sign in; new, edit, duplicate, delete', async ({ app }) => {
    const dialog = await openCredentials(app);
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('deploy');
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('password stored · key work-laptop');

    // New: needs a name, a user and a way to sign in; a stored password needs the password.
    await dialog.getByRole('button', { name: 'New login…' }).click();
    const form = app.getByRole('dialog', { name: 'New login' });
    const add = form.getByRole('button', { name: 'Add login' });
    await form.getByRole('textbox', { name: 'Login name' }).fill('DEPLOY@PROD');
    await form.getByRole('textbox', { name: 'User name' }).fill('admin');
    await expect(form).toContainText('A login with this name already exists');
    await form.getByRole('textbox', { name: 'Login name' }).fill('admin@lab');
    await expect(add).toBeDisabled();  // "Stored" without a password
    await form.getByRole('radiogroup', { name: 'Password' }).getByRole('radio', { name: 'None' }).click();
    await expect(form).toContainText('Choose at least one way to sign in');
    await form.getByRole('radiogroup', { name: 'Password' }).getByRole('radio', { name: 'Stored' }).click();
    await form.getByRole('textbox', { name: 'Password to store' }).fill('s3cret');
    await form.getByRole('combobox', { name: 'Login key' }).selectOption({ label: 'router · rsa' });
    await add.click();
    await expect(form).toBeHidden();
    await expect(loginRow(dialog, 'admin@lab')).toContainText('password stored · key router');

    // Edit: the stored password stays unless a new one is typed; switch to "Ask every time".
    await loginRow(dialog, 'admin@lab').getByRole('button', { name: 'Edit' }).click();
    const editForm = app.getByRole('dialog', { name: 'Edit login "admin@lab"' });
    await expect(editForm.getByRole('textbox', { name: 'Password to store' })).toHaveAttribute('placeholder', '(unchanged)');
    await editForm.getByRole('radiogroup', { name: 'Password' }).getByRole('radio', { name: 'Ask every time' }).click();
    await editForm.getByRole('button', { name: 'Save' }).click();
    await expect(loginRow(dialog, 'admin@lab')).toContainText('password asked · key router');

    // Duplicate, then delete the copy.
    await loginRow(dialog, 'admin@lab').getByRole('button', { name: 'Duplicate' }).click();
    const dup = app.getByRole('dialog', { name: 'New login' });
    await expect(dup.getByRole('textbox', { name: 'Login name' })).toHaveValue('admin@lab (copy)');
    await dup.getByRole('button', { name: 'Add login' }).click();
    await expect(loginRow(dialog, 'admin@lab (copy)')).toBeVisible();
    await loginRow(dialog, 'admin@lab (copy)').getByRole('button', { name: 'Delete' }).click();
    await app.getByRole('dialog', { name: 'Delete login "admin@lab (copy)"?' }).getByRole('button', { name: 'Delete' }).click();
    await expect(loginRow(dialog, 'admin@lab (copy)')).toHaveCount(0);
  });

  test('a session signs in with a saved login: its own key, agent and password settings step aside', async ({ app }) => {
    const editor = await editSession(app, 'web-01');
    await editorPage(editor, 'Authentication');
    await expect(editor.getByRole('heading', { name: 'Private key' })).toBeVisible();
    await editor.getByRole('radiogroup', { name: 'Sign in with' }).getByRole('radio', { name: 'A saved login' }).click();
    await editor.getByRole('combobox', { name: 'Saved login' }).selectOption({ label: 'deploy@prod (deploy)' });
    await expect(editor).toContainText('User deploy · password stored · key work-laptop');
    await expect(editor.getByRole('heading', { name: 'Private key' })).toHaveCount(0);
    await expect(editor.getByRole('heading', { name: 'Saved password' })).toHaveCount(0);
    await expect(editor.getByText('Try authentication using an SSH agent')).toHaveCount(0);
    await expect(editor.getByText('Allow agent forwarding')).toBeVisible();  // still the connection's
    await editor.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(editor).toBeHidden();

    const again = await editSession(app, 'web-01');
    await expect(again).toContainText("Replaced by the login's user name (deploy)");
    await editorPage(again, 'Authentication');
    await expect(again.getByRole('combobox', { name: 'Saved login' })).toHaveValue('login-deploy');
    // Back to the session's own settings.
    await again.getByRole('radiogroup', { name: 'Sign in with' }).getByRole('radio', { name: "This session's settings" }).click();
    await expect(again.getByRole('heading', { name: 'Private key' })).toBeVisible();
    await again.getByRole('button', { name: 'Cancel' }).click();
    await app.getByRole('dialog', { name: /Discard/ }).getByRole('button', { name: 'Discard changes' }).click();

    const dialog = await openCredentials(app);
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('1 session');
    // The key "work-laptop" is used by the login: its delete question says so.
    await dialog.locator('.vault-row', { hasText: 'work-laptop' }).getByRole('button', { name: 'Delete' }).click();
    await expect(app.getByRole('dialog', { name: 'Delete "work-laptop"?' })).toContainText('login deploy@prod');
  });

  test('"Use login" in the sidebar: for one session and for every SSH session of a folder', async ({ app }) => {
    await sessionItem(app, 'db-primary').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Use login' }).hover();
    await app.getByRole('menuitem', { name: 'deploy@prod (deploy)' }).click();

    await app.getByRole('tree', { name: 'Sessions' }).getByRole('treeitem', { name: /^Web/ }).click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Use login for 2 SSH sessions' }).hover();
    await app.getByRole('menuitem', { name: 'deploy@prod (deploy)' }).click();

    const dialog = await openCredentials(app);
    await expect(loginRow(dialog, 'deploy@prod')).toContainText('3 sessions');
    await dialog.getByRole('button', { name: 'Close' }).first().click();

    // The check mark shows the login in use; "This session's settings" clears it.
    await sessionItem(app, 'db-primary').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Use login' }).hover();
    await app.getByRole('menuitem', { name: "This session's settings" }).click();
    const again = await openCredentials(app);
    await expect(loginRow(again, 'deploy@prod')).toContainText('2 sessions');
  });
});
