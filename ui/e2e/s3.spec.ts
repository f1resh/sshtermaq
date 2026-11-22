import { expect, test } from './fixtures';
import { editorPage, sessionItem } from './helpers';
import type { Locator, Page } from '@playwright/test';

/**
 * S3 sessions against the mock backend (src/bridge/mock/sftp.ts, buildBuckets): a saved S3 session opens a file
 * manager tab whose root lists the buckets, without permissions or a terminal; "Copy link…" makes a presigned link;
 * the session editor has the S3 type; Quick connect opens s3://bucket/folder.
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
