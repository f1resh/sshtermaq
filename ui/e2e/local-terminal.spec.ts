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
