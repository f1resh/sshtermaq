import { expect, test } from './fixtures';

test.describe('welcome screen and window', () => {
  test('lists the recent sessions and quick connect', async ({ app }) => {
    const main = app.getByRole('main');
    await expect(main.getByRole('heading', { name: 'SSHTermaq', level: 1 })).toBeVisible();
    await expect(main.getByRole('heading', { name: 'Recent sessions' })).toBeVisible();
    for (const name of ['web-01', 'db-primary', 'router', 'Console COM3'])
      await expect(main.getByRole('button', { name: new RegExp(`^${name}\\b`) })).toBeVisible();
    await expect(main.getByRole('button', { name: 'Connect' })).toBeDisabled();
  });

  test('quick connect shows the serial example of the system', async ({ app, os }) => {
    const input = app.getByRole('main').getByRole('textbox').first();
    const placeholder = await input.getAttribute('placeholder');
    if (os === 'darwin') {
      expect(placeholder).toContain('/dev/cu.');
      expect(placeholder).not.toContain('COM3');
    } else {
      expect(placeholder).toContain('serial://COM3');
    }
  });

  test('title bar: caption buttons and app menu on Windows, none on macOS', async ({ app, os }) => {
    const banner = app.getByRole('banner');
    const windowButtons = [
      banner.getByRole('button', { name: 'Minimize' }),
      banner.getByRole('button', { name: 'Maximize' }),
      banner.getByRole('button', { name: 'Close', exact: true }),
      banner.getByRole('button', { name: 'Application menu' })
    ];
    for (const button of windowButtons) {
      if (os === 'darwin') await expect(button).toHaveCount(0);   // traffic lights and the menu bar are native
      else await expect(button).toBeVisible();
    }
  });

  test('the sidebar can be hidden and shown again', async ({ app }) => {
    const toggle = app.getByRole('button', { name: /session sidebar/ });
    const sidebar = app.getByRole('complementary', { name: 'Saved sessions' });
    await toggle.click();
    await expect(sidebar).toBeHidden();
    await app.getByRole('button', { name: /session sidebar/ }).click();
    await expect(sidebar).toBeVisible();
  });
});
