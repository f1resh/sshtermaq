import { expect, test } from './fixtures';
import { editSession, editorPage, sessionItem } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * S3 sessions against the mock backend (src/bridge/mock/sftp.ts, buildBuckets): a saved S3 session opens a file
 * manager tab whose root lists the buckets, without permissions or a terminal; "Copy link…" makes a presigned link;
 * the session editor has the S3 type, and its access key can come from a saved login; Quick connect opens
 * s3://bucket/folder.
 */
const server = (page: Page) => page.locator('section[data-pane-side="remote"]');
const item = (pane: Locator, path: string) => pane.getByTitle(path, { exact: true });

async function openStorage(page: Page): Promise<Locator> {
  await sessionItem(page, 'media (S3)').dblclick();
  const s = server(page);
  await expect(item(s, '/photos-2026')).toBeVisible();
  return s;
}

test.describe('S3', () => {
  test('a saved S3 session opens the buckets in a file manager tab', async ({ app }) => {
    const s = await openStorage(app);
    await expect(app.locator('.fm-badge--s3')).toHaveText('S3');
    await expect(item(s, '/backups')).toBeVisible();
    await expect(item(s, '/empty-bucket')).toBeVisible();
    // No Permissions column and no "terminal to this server" on S3.
    await expect(s.getByRole('button', { name: 'Permissions' })).toHaveCount(0);
    await expect(app.getByRole('button', { name: 'Open a terminal to this server' })).toHaveCount(0);

    // The list of buckets cannot be changed here.
    await expect(s.getByRole('button', { name: /^New folder/ })).toBeDisabled();
    await item(s, '/backups').click({ button: 'right' });
    await expect(app.getByRole('menuitem', { name: 'Rename…' })).toBeDisabled();
    await expect(app.getByRole('menuitem', { name: /^Delete/ })).toBeDisabled();
    await expect(app.getByRole('menuitem', { name: 'Permissions…' })).toHaveCount(0);
    await app.keyboard.press('Escape');

    // Inside a bucket folders work as usual.
    await item(s, '/photos-2026').dblclick();
    await expect(s).toHaveAttribute('data-pane-path', '/photos-2026');
    await expect(item(s, '/photos-2026/january')).toBeVisible();
    await expect(s.getByRole('button', { name: /^New folder/ })).toBeEnabled();
  });

  test('Copy link… makes a time-limited link', async ({ app }) => {
    const s = await openStorage(app);
    await item(s, '/photos-2026').dblclick();
    await item(s, '/photos-2026/index.html').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Copy link…' }).click();
    const dialog = app.getByRole('dialog', { name: 'Link to index.html' });
    await expect(dialog.getByRole('combobox', { name: 'Valid for' })).toHaveValue('3600');
    await dialog.getByRole('combobox', { name: 'Valid for' }).selectOption({ label: 'Other…' });
    await dialog.getByRole('textbox', { name: 'Amount' }).fill('8');
    await dialog.getByRole('combobox', { name: 'Unit' }).selectOption('days');
    await expect(dialog.getByText('Between 1 minute and 7 days')).toBeVisible();
    await expect(dialog.getByRole('button', { name: 'Copy link' })).toBeDisabled();
    await dialog.getByRole('textbox', { name: 'Amount' }).fill('2');
    await dialog.getByRole('button', { name: 'Copy link' }).click();
    await expect(dialog.getByRole('textbox', { name: 'Link' })).toHaveValue(/X-Amz-Expires=172800/);
    const copied = await app.evaluate(() => (globalThis as { __mockClipboard?: string }).__mockClipboard);
    expect(copied).toContain('photos-2026.s3.us-east-1.amazonaws.com/index.html');

    // Folders have no links.
    await dialog.getByText('Close', { exact: true }).click();
    await item(s, '/photos-2026/january').click({ button: 'right' });
    await expect(app.getByRole('menuitem', { name: 'Copy link…' })).toBeDisabled();
  });

  test('Copy s3:// path gives the full path of buckets, folders and files', async ({ app }) => {
    const s = await openStorage(app);
    const clipboard = () => app.evaluate(() => (globalThis as { __mockClipboard?: string }).__mockClipboard);
    const copy = async (target: Locator, label: string) => {
      await target.click({ button: 'right' });
      await app.getByRole('menuitem', { name: label, exact: true }).click();
    };

    // The list of buckets: a bucket, and nothing for the list itself.
    await copy(item(s, '/photos-2026'), 'Copy s3:// path');
    await expect.poll(clipboard).toBe('s3://photos-2026/');
    await s.locator('.fm-list').click({ button: 'right', position: { x: 10, y: 400 } });
    await expect(app.getByRole('menuitem', { name: 'Copy current s3:// path' })).toBeDisabled();
    await app.keyboard.press('Escape');

    // Inside a bucket: a file, a folder (ends with "/"), several at once, the current folder.
    await item(s, '/photos-2026').dblclick();
    await copy(item(s, '/photos-2026/index.html'), 'Copy s3:// path');
    await expect.poll(clipboard).toBe('s3://photos-2026/index.html');
    await copy(item(s, '/photos-2026/january'), 'Copy s3:// path');
    await expect.poll(clipboard).toBe('s3://photos-2026/january/');
    await item(s, '/photos-2026/january').click();
    await item(s, '/photos-2026/index.html').click({ modifiers: ['Shift'] });  // folders first: january, index.html
    await copy(item(s, '/photos-2026/index.html'), 'Copy s3:// paths');
    await expect.poll(clipboard).toMatch(/^s3:\/\/photos-2026\/(january\/\r\ns3:\/\/photos-2026\/index\.html|index\.html\r\ns3:\/\/photos-2026\/january\/)$/);
    await item(s, '/photos-2026/january').dblclick();
    await expect(s).toHaveAttribute('data-pane-path', '/photos-2026/january');
    await s.locator('.fm-list').click({ button: 'right', position: { x: 10, y: 400 } });
    await app.getByRole('menuitem', { name: 'Copy current s3:// path' }).click();
    await expect.poll(clipboard).toBe('s3://photos-2026/january/');

    // "Copy path" is still the path in the pane.
    await copy(item(s, '/photos-2026/january/IMG_0001.jpg'), 'Copy path');
    await expect.poll(clipboard).toBe('/photos-2026/january/IMG_0001.jpg');
  });

  test('the session editor has the S3 type with service presets and sign-in methods', async ({ app }) => {
    await app.getByRole('button', { name: 'New session' }).first().click();
    const dialog = app.getByRole('dialog', { name: /session/i });
    await dialog.getByRole('radio', { name: 'S3' }).click();
    await expect(dialog.getByRole('navigation', { name: 'Session categories' }).getByRole('button', { name: 'Terminal', exact: true })).toHaveCount(0);
    await dialog.getByRole('combobox', { name: 'Service' }).selectOption({ label: 'MinIO' });
    await expect(dialog.getByLabel('Endpoint')).toHaveValue('http://localhost:9000');
    await expect(dialog.getByText('Plain HTTP: files and requests travel unencrypted')).toBeVisible();
    await dialog.getByLabel('Bucket').fill('Bad Name');
    await expect(dialog.getByText('Not a valid bucket name')).toBeVisible();
    await dialog.getByLabel('Bucket').fill('media');

    await editorPage(dialog, 'S3');
    await dialog.getByRole('radio', { name: 'AWS profile' }).click();
    await dialog.getByLabel('Profile').fill('dev');
    await dialog.getByRole('radio', { name: 'Access key' }).click();
    await dialog.getByLabel('Access key ID').fill('AKIAEXAMPLE');
    await expect(dialog.getByRole('heading', { name: 'Saved secret access key' })).toBeVisible();
  });

  test('the access key can come from a saved login in Credentials', async ({ app }) => {
    const accessKeyFrom = (d: Locator) => d.getByRole('radiogroup', { name: 'Access key from' });
    const editor = await editSession(app, 'media (S3)');
    await editorPage(editor, 'S3');
    // As before: the session's own access key.
    await expect(accessKeyFrom(editor).getByRole('radio', { name: 'This session', checked: true })).toBeVisible();
    await expect(editor.getByLabel('Access key ID')).toHaveValue('AKIAMOCKEXAMPLE0001');

    await accessKeyFrom(editor).getByRole('radio', { name: 'A saved login' }).click();
    await expect(editor.getByLabel('Access key ID')).toHaveCount(0);
    const saved = editor.getByRole('combobox', { name: 'Saved login' });
    await expect(saved).toHaveValue('');
    await saved.selectOption({ label: 'deploy@prod (deploy)' });
    await editor.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(editor).toBeHidden();

    // Opened again: the login is still chosen.
    const again = await editSession(app, 'media (S3)');
    await editorPage(again, 'S3');
    await expect(accessKeyFrom(again).getByRole('radio', { name: 'A saved login', checked: true })).toBeVisible();
    await expect(again.getByRole('combobox', { name: 'Saved login' })).toHaveValue('login-deploy');
    await again.getByRole('button', { name: 'Cancel' }).click();

    // Credentials counts the S3 session among the login's users, and the delete question names it.
    await app.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
    const settings = app.getByRole('dialog', { name: 'Settings' });
    await settings.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Credentials', exact: true }).click();
    const row = settings.locator('.vault-login-row', { hasText: 'deploy@prod' });
    await expect(row).toContainText('1 session');
    await row.getByRole('button', { name: 'Delete' }).click();
    const question = app.getByRole('dialog', { name: 'Delete login "deploy@prod"?' });
    await expect(question).toContainText('Used by 1 session');
    await expect(question).toContainText('media (S3)');
    await expect(question).toContainText('They will sign in with their own settings again.');
    await question.getByRole('button', { name: 'Delete' }).click();
    await settings.getByRole('button', { name: 'Close' }).first().click();
    const gone = await editSession(app, 'media (S3)');
    await editorPage(gone, 'S3');
    await expect(gone.getByText('The chosen login is no longer in Credentials, so this session signs in with its own access key. Choose another login.')).toBeVisible();

    // Back to "This session": the login is cleared and the access key field comes back.
    await accessKeyFrom(gone).getByRole('radio', { name: 'This session' }).click();
    await expect(gone.getByLabel('Access key ID')).toHaveValue('AKIAMOCKEXAMPLE0001');
    await expect(gone.getByText('The chosen login is no longer in Credentials')).toHaveCount(0);
    await gone.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(gone).toBeHidden();
    const own = await editSession(app, 'media (S3)');
    await editorPage(own, 'S3');
    await expect(accessKeyFrom(own).getByRole('radio', { name: 'This session', checked: true })).toBeVisible();
  });

  test('Quick connect opens s3://bucket/folder', async ({ app }) => {
    await app.getByRole('button', { name: 'New tab options' }).click();
    await app.getByRole('menuitem', { name: /Quick connect/ }).click();
    const dialog = app.getByRole('dialog', { name: 'Quick connect' });
    await dialog.getByRole('textbox').fill('s3://photos-2026/january');
    await expect(dialog.getByRole('radio', { name: 'S3', checked: true })).toBeVisible();
    await dialog.getByRole('button', { name: 'Connect' }).click();
    await expect(app.locator('.fm-badge--s3')).toBeVisible();
  });
});
