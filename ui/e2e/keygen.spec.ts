import { expect, test } from './fixtures';
import { mockDialog, openPalette, savedFiles } from './helpers';
import type { Page } from '@playwright/test';

/** The key generator against the mock (src/bridge/mock/keys.ts): it makes keys of the right shape, not real ones. */
async function openKeygen(page: Page, os: 'win32' | 'darwin') {
  const palette = await openPalette(page, os);
  await page.keyboard.type('key generator');
  await palette.getByRole('option', { name: /Key generator/ }).click();
  const dialog = page.getByRole('dialog', { name: 'Key generator' });
  await expect(dialog).toBeVisible();
  return dialog;
}

test.describe('key generator', () => {
  test('generates an Ed25519 key: public key, fingerprint, and a warning without a passphrase', async ({ app, os }) => {
    const dialog = await openKeygen(app, os);
    await expect(dialog.getByRole('radiogroup', { name: 'Key type' }).getByRole('radio', { name: 'Ed25519' })).toBeChecked();
    await expect(dialog).toContainText('Without a passphrase, anyone who gets hold of the private key file can use it.');
    await dialog.getByRole('button', { name: 'Generate', exact: true }).click();
    await expect(dialog.getByText('Your new key')).toBeVisible();
    await expect(dialog).toContainText(/SHA256:/);
    await expect(dialog.getByRole('textbox', { name: /Public key/ }).or(dialog.locator('textarea')).first()).toHaveValue(/^ssh-ed25519 AAAA/);
    await expect(dialog.getByRole('button', { name: 'Generate another' })).toBeVisible();
  });

  test('RSA offers key sizes, ECDSA curves', async ({ app, os }) => {
    const dialog = await openKeygen(app, os);
    const types = dialog.getByRole('radiogroup', { name: 'Key type' });
    await types.getByRole('radio', { name: 'RSA' }).click();
    await expect(dialog.getByText('Key size (bits)')).toBeVisible();
    const size = dialog.getByRole('radiogroup', { name: 'Key size' });
    await expect(size.getByRole('radio')).toHaveText(['2048', '3072', '4096']);
    await types.getByRole('radio', { name: 'ECDSA' }).click();
    await expect(dialog.getByText('Curve', { exact: true })).toBeVisible();
    await expect(size.getByRole('radio')).toHaveText(['P-256', 'P-384', 'P-521']);
    await size.getByRole('radio', { name: 'P-384' }).click();
    await dialog.getByRole('button', { name: 'Generate', exact: true }).click();
    await expect(dialog.locator('textarea').first()).toHaveValue(/^ecdsa-sha2-nistp384 /);
  });

  test('the passphrase must be confirmed and match', async ({ app, os }) => {
    const dialog = await openKeygen(app, os);
    const [pass, confirm] = [dialog.locator('input[type="password"]').nth(0), dialog.locator('input[type="password"]').nth(1)];
    const generate = dialog.getByRole('button', { name: 'Generate', exact: true });
    await pass.fill('correct horse');
    await expect(generate).toBeDisabled();
    await expect(dialog).toContainText('Confirm the passphrase to continue.');
    await confirm.fill('correct hors');
    await expect(dialog).toContainText('Passphrases do not match');
    await expect(generate).toBeDisabled();
    await confirm.fill('correct horse');
    await expect(generate).toBeEnabled();
    await expect(dialog).not.toContainText('Without a passphrase');
    await generate.click();
    await expect(dialog.getByText('Your new key')).toBeVisible();
  });

  test('saves the private key (OpenSSH, .ppk) owner-only and the public key; copies the public key', async ({ app, os }) => {
    const dialog = await openKeygen(app, os);
    await dialog.getByRole('button', { name: 'Generate', exact: true }).click();
    await expect(dialog.getByText('Your new key')).toBeVisible();

    const dir = os === 'darwin' ? '/Users/demo/.ssh/' : 'C:\\Users\\demo\\.ssh\\';
    await mockDialog(app, 'saveTextFile', `${dir}id_ed25519`);
    await dialog.getByRole('button', { name: 'Save private key (OpenSSH)' }).click();
    await mockDialog(app, 'saveTextFile', `${dir}id_ed25519.ppk`);
    await dialog.getByRole('button', { name: 'Save PuTTY .ppk' }).click();
    await mockDialog(app, 'saveTextFile', `${dir}id_ed25519.pub`);
    await dialog.getByRole('button', { name: 'Save public key' }).click();

    await expect.poll(async () => (await savedFiles(app)).length).toBe(3);
    const [openssh, ppk, pub] = await savedFiles(app);
    expect(openssh.content).toContain('-----BEGIN OPENSSH PRIVATE KEY-----');
    expect(openssh.secret).toBe(true);
    expect(ppk.content).toMatch(/^PuTTY-User-Key-File-3: ssh-ed25519/);
    expect(ppk.secret).toBe(true);
    expect(pub.content).toMatch(/^ssh-ed25519 AAAA/);
    expect(pub.secret).toBe(false);
    await expect(app.getByRole('status')).toContainText('Saved');

    // A cancelled Save dialog saves nothing.
    await dialog.getByRole('button', { name: 'Save public key' }).click();
    await expect.poll(async () => (await savedFiles(app)).length).toBe(3);

    await dialog.getByRole('button', { name: 'Copy public key' }).click();
    await expect.poll(() => app.evaluate(() => (globalThis as { __mockClipboard?: string }).__mockClipboard ?? '')).toMatch(/^ssh-ed25519 AAAA/);
  });

  test('loads an existing key: asks for the passphrase of an encrypted one, then converts it', async ({ app, os }) => {
    const dialog = await openKeygen(app, os);
    await dialog.getByRole('radiogroup', { name: 'Mode' }).getByRole('radio', { name: 'Load existing key' }).click();
    // The generate page stays in the page (hidden): work inside the "Key file" section.
    const keyFile = dialog.locator('section', { hasText: 'PuTTY .ppk (v2 / v3), OpenSSH or PEM private key.' });
    const path = keyFile.getByRole('textbox').first();

    await path.fill(os === 'darwin' ? '/Users/demo/keys/missing.pem' : 'C:\\keys\\missing.pem');
    await dialog.getByRole('button', { name: 'Load', exact: true }).click();
    await expect(dialog).toContainText('Cannot use this key');

    await path.fill(os === 'darwin' ? '/Users/demo/keys/server-enc.pem' : 'C:\\keys\\server-enc.pem');
    await dialog.getByRole('button', { name: 'Load', exact: true }).click();
    await expect(dialog).toContainText('Passphrase required');
    const pass = keyFile.locator('input[type="password"]');
    await pass.fill('secret');
    await pass.press('Enter');
    await expect(dialog).toContainText('Passphrase protected');
    await expect(dialog).toContainText(/SHA256:/);

    await mockDialog(app, 'saveTextFile', os === 'darwin' ? '/Users/demo/keys/server.ppk' : 'C:\\keys\\server.ppk');
    await dialog.getByRole('button', { name: 'Convert to .ppk…' }).click();
    await expect.poll(async () => (await savedFiles(app)).at(-1)?.content ?? '').toMatch(/^PuTTY-User-Key-File-3: /);
  });
});
