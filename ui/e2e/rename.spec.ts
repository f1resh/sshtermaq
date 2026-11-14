import { expect, test } from './fixtures';
import { openPalette, sessionItem, terminalText, useDomRenderer } from './helpers';

test.describe('renaming tabs and panes', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
    await sessionItem(app, 'web-01').dblclick();
    await expect(terminalText(app)).toContainText('deploy@web-01');
  });

  test('a split pane is renamed by double-clicking its header, and names the tab', async ({ app, os }) => {
    const palette = await openPalette(app, os);
    await app.keyboard.type('split right');
    await palette.getByRole('option', { name: /Split pane right/ }).click();
    const titles = app.locator('.pane-head__title');
    await expect(titles).toHaveCount(2);
    await titles.first().dblclick();
    const field = app.getByRole('textbox', { name: 'Pane name' });
    await field.fill('logs');
    await field.press('Enter');
    await expect(app.locator('.pane-head__title', { hasText: 'logs' })).toBeVisible();
    await app.locator('.pane-head', { hasText: 'logs' }).click();
    await expect(app.getByRole('tab', { name: /logs/, selected: true })).toBeVisible();
  });

  test('the tab menu renames the tab; an empty name brings the automatic title back', async ({ app }) => {
    const tab = app.getByRole('tab', { name: /web-01/ });
    await tab.click({ button: 'right' });
    await app.getByRole('menuitem', { name: /Rename/ }).click();
    const field = app.getByRole('textbox', { name: 'Tab title' });
    await field.fill('Production');
    await field.press('Enter');
    await expect(app.getByRole('tab', { name: /Production/ })).toBeVisible();
    await app.getByRole('tab', { name: /Production/ }).dblclick();
    await app.getByRole('textbox', { name: 'Tab title' }).fill('');
    await app.getByRole('textbox', { name: 'Tab title' }).press('Enter');
    await expect(app.getByRole('tab', { name: /web-01/ })).toBeVisible();
  });
});
