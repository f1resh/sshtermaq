import { expect, test } from './fixtures';
import { confirmClose, openPalette, sessionItem, terminalText, useDomRenderer } from './helpers';

test.describe('connecting and terminals', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
  });

  test('double-clicking a session opens a tab with the shell', async ({ app }) => {
    await sessionItem(app, 'web-01').dblclick();
    await expect(app.getByRole('tab', { name: /web-01/, selected: true })).toBeVisible();
    await expect(terminalText(app)).toContainText('Welcome to Ubuntu');
    await expect(terminalText(app)).toContainText('deploy@web-01');
  });

  test('typed commands reach the shell and its output is shown', async ({ app }) => {
    await sessionItem(app, 'web-01').dblclick();
    await expect(terminalText(app)).toContainText('deploy@web-01');
    await app.locator('.xterm-helper-textarea').first().focus();
    await app.keyboard.type('help');
    await app.keyboard.press('Enter');
    await expect(terminalText(app)).toContainText('Commands: help');
  });

  test('closing the tab returns to the welcome screen', async ({ app }) => {
    await sessionItem(app, 'web-01').dblclick();
    const tab = app.getByRole('tab', { name: /web-01/ });
    await expect(tab).toBeVisible();
    await tab.getByRole('button', { name: 'Close tab' }).click();
    await confirmClose(app);   // a connected session asks first
    await expect(tab).toHaveCount(0);
    await expect(app.getByRole('heading', { name: 'Recent sessions' })).toBeVisible();
  });

  test('quick connect opens a session from user@host', async ({ app }) => {
    const input = app.getByRole('main').getByRole('textbox').first();
    await input.fill('admin@quick.example.com');
    await app.getByRole('main').getByRole('button', { name: 'Connect' }).click();
    await expect(app.getByRole('tab', { name: /quick\.example\.com/ })).toBeVisible();
    await expect(terminalText(app)).toContainText('admin@');
  });

  test('split right from the command palette makes two panes', async ({ app, os }) => {
    await sessionItem(app, 'web-01').dblclick();
    await expect(terminalText(app)).toContainText('deploy@web-01');
    const palette = await openPalette(app, os);
    await app.keyboard.type('split right');
    await expect(palette.getByRole('option', { name: /Split pane right/ })).toBeVisible();
    await app.keyboard.press('Enter');
    await expect(palette).toBeHidden();
    const panes = app.getByRole('tabpanel').getByRole('button', { name: 'Close pane' });
    await expect(panes).toHaveCount(2);
    await panes.first().click();
    await confirmClose(app);
    await expect.poll(() => panes.count()).toBeLessThan(2);
  });
});
