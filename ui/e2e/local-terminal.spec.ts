import { expect, test } from './fixtures';
import { terminalText, useDomRenderer } from './helpers';

/** The mock's shells (src/bridge/mock/core.ts): PowerShell 7 is the default on Windows, zsh on macOS. */
const SHELLS = {
  win32: { default: 'pwsh', other: 'Git Bash', otherTab: 'bash' },
  darwin: { default: 'zsh', other: 'bash', otherTab: 'bash' }
} as const;

test.describe('local terminals', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
  });

  test('the shortcut (macOS: the File menu) opens the default shell', async ({ app, os }) => {
    if (os === 'darwin')
      await app.evaluate(() => (globalThis as unknown as { __mockEmit(ch: string, ...a: unknown[]): void }).__mockEmit('window:onMenuCommand', 'tab.newLocal'));
    else await app.keyboard.press('Control+Shift+Backquote');
    await expect(app.getByRole('tab', { name: new RegExp(SHELLS[os].default), selected: true })).toBeVisible();
    await expect(terminalText(app)).toContainText('Welcome');
  });

  test('the new tab menu lists the installed shells and opens one', async ({ app, os }) => {
    await app.getByRole('button', { name: 'New tab options' }).click();
    await expect(app.getByRole('menuitem', { name: /Quick connect/ })).toBeVisible();
    await app.getByRole('menuitem', { name: SHELLS[os].other, exact: true }).click();
    await expect(app.getByRole('tab', { name: new RegExp(SHELLS[os].otherTab), selected: true })).toBeVisible();
    await expect(terminalText(app)).toContainText('Welcome');
  });

  test('the welcome screen has a Local terminal button', async ({ app, os }) => {
    await app.getByRole('main').getByRole('button', { name: 'Local terminal' }).click();
    await expect(app.getByRole('tab', { name: new RegExp(SHELLS[os].default) })).toBeVisible();
  });
});

test.describe('local terminals from quick connect', () => {
  test('"local:" plus a shell name opens that shell', async ({ app, os }) => {
    const input = app.getByRole('main').getByRole('textbox').first();
    await input.fill('local:bash');
    await input.press('Enter');
    await expect(app.getByRole('tab', { name: /bash/, selected: true })).toBeVisible();
    // The Local chip of the quick connect dialog shows which shell a name picks.
    await app.getByRole('button', { name: 'New tab options' }).click();
    await app.getByRole('menuitem', { name: /Quick connect/ }).click();
    const dialog = app.getByRole('dialog', { name: 'Quick connect' });
    await dialog.getByRole('radio', { name: 'Local' }).click();
    await dialog.getByRole('textbox').fill(os === 'darwin' ? 'zsh' : 'git');
    await expect(dialog).toContainText(os === 'darwin' ? 'Local · zsh' : 'Local · Git Bash');
  });
});
