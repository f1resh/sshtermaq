import { expect, test } from './fixtures';
import { openPalette, sessionItem } from './helpers';

test.describe('sidebar and command palette', () => {
  test('search filters the saved sessions', async ({ app }) => {
    await app.getByRole('textbox', { name: 'Search sessions' }).fill('raspberry');
    await expect(sessionItem(app, 'raspberrypi')).toBeVisible();
    await expect(sessionItem(app, 'web-01')).toHaveCount(0);
    await app.getByRole('button', { name: 'Clear search' }).click();
    await expect(sessionItem(app, 'web-01')).toBeVisible();
  });

  test('the context menu offers the session actions', async ({ app }) => {
    await sessionItem(app, 'web-01').click({ button: 'right' });
    for (const item of ['Connect', 'Connect in split right', 'Edit…', 'Duplicate', 'Rename…', 'Delete'])
      await expect(app.getByRole('menuitem', { name: new RegExp(`^${item}( \\S+)?$`) })).toBeVisible();   // "Rename… F2"
    await app.keyboard.press('Escape');
    await expect(app.getByRole('menuitem', { name: 'Edit…' })).toHaveCount(0);
  });

  test('duplicate adds a copy of the session', async ({ app }) => {
    await sessionItem(app, 'web-01').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Duplicate', exact: true }).click();
    await expect(sessionItem(app, 'web-01 (copy)')).toBeVisible();
    await expect(sessionItem(app, 'web-01')).toBeVisible();
  });

  test('the command palette opens (shortcut, or the macOS menu bar) and closes with Escape', async ({ app, os }) => {
    const palette = await openPalette(app, os);
    await app.keyboard.type('@web');
    await expect(palette.getByRole('option', { name: /web-01/ })).toBeVisible();
    await app.keyboard.press('Escape');
    await expect(palette).toBeHidden();
  });

  test('the welcome screen shows the system\'s shortcuts', async ({ app, os }) => {
    const hints = app.getByRole('main');
    if (os === 'darwin') await expect(hints).toContainText('⌘');
    else await expect(hints).toContainText('Ctrl+Shift+P');
  });
});
