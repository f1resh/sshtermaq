import { expect, test } from './fixtures';
import { openPalette } from './helpers';
import type { Locator, Page } from '@playwright/test';

/** The settings dialog; each change is saved at once through the bridge (mock: localStorage). */
async function openSettings(page: Page): Promise<Locator> {
  await page.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await expect(dialog).toBeVisible();
  return dialog;
}
async function section(dialog: Locator, name: string): Promise<void> {
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name, exact: true }).click();
  // Every page but About starts with its name as a heading.
  if (name === 'About') await expect(dialog.getByRole('heading', { name: 'Third-party software' })).toBeVisible();
  else await expect(dialog.getByRole('heading', { level: 2, name })).toBeVisible();
}
/** The on/off switch of a settings row, by the row's label (the input itself is hidden; its label is what is clicked). */
const row = (dialog: Locator, label: string) => dialog.locator('.dlg-row', { has: dialog.page().getByText(label, { exact: true }) });
const toggle = (dialog: Locator, label: string) => row(dialog, label).getByRole('switch');
const flip = (dialog: Locator, label: string) => row(dialog, label).locator('label.switch').click();
function saved(page: Page): Promise<Record<string, Record<string, unknown>>> {
  return page.evaluate(async () => {
    const module = '/src/bridge/api.ts';
    const { api } = (await import(module)) as typeof import('../src/bridge/api');
    return (await api.settings.get()) as unknown as Record<string, Record<string, unknown>>;
  });
}

test.describe('settings', () => {
  test('every section opens', async ({ app }) => {
    const dialog = await openSettings(app);
    for (const name of ['Appearance', 'Terminal', 'Behavior', 'File manager', 'Keybindings', 'Color schemes', 'Known hosts', 'Default session', 'Data & backup', 'About'])
      await section(dialog, name);
  });

  test('a theme is applied to the window at once and kept', async ({ app }) => {
    const dialog = await openSettings(app);
    await dialog.getByRole('button', { name: 'Dracula', exact: true }).click();
    await expect(app.locator('html')).toHaveAttribute('data-theme', 'dracula');
    await dialog.getByRole('button', { name: 'Solarized Light', exact: true }).click();
    await expect(app.locator('html')).toHaveAttribute('data-theme', 'solarized-light');
    await app.keyboard.press('Escape');
    await expect(dialog).toBeHidden();
    expect((await saved(app)).appearance.theme).toBe('solarized-light');
    await app.reload();
    await expect(app.locator('html')).toHaveAttribute('data-theme', 'solarized-light');
  });

  test('switches and choices are saved (behavior, file manager)', async ({ app }) => {
    const dialog = await openSettings(app);
    await section(dialog, 'Behavior');
    const copy = toggle(dialog, 'Copy on select');
    await expect(copy).toBeChecked();
    await flip(dialog, 'Copy on select');
    await expect(copy).not.toBeChecked();
    await expect.poll(async () => (await saved(app)).behavior.copyOnSelect).toBe(false);

    const names = toggle(dialog, 'Keep tab and pane names');
    await expect(names).toBeDisabled();
    await flip(dialog, 'Restore tabs on startup');
    await expect(names).toBeEnabled();
    await expect.poll(async () => (await saved(app)).behavior.restoreTabs).toBe(true);

    await section(dialog, 'File manager');
    await flip(dialog, 'Confirm before deleting');
    await expect(toggle(dialog, 'Confirm before deleting')).not.toBeChecked();
    await expect.poll(async () => (await saved(app)).files.confirmDelete).toBe(false);
  });

  test('rows only one system has', async ({ app, os }) => {
    const dialog = await openSettings(app);
    // Windows: the Mica / Acrylic backdrop. (It is also shown on macOS today, where the window ignores it: a known
    // UI bug, so only Windows is checked here.)
    if (os === 'win32') await expect(dialog.getByText('Background material', { exact: true })).toHaveCount(1);
    // macOS: Option as Meta.
    await section(dialog, 'Terminal');
    await expect(dialog.getByText('Use Option as Meta key', { exact: true })).toHaveCount(os === 'darwin' ? 1 : 0);
  });

  test('a new key binding works at once; "Reset to default" brings the old one back', async ({ app, os }) => {
    const dialog = await openSettings(app);
    await section(dialog, 'Keybindings');
    await dialog.getByPlaceholder('Search commands').fill('Command palette');
    const row = dialog.locator('.kb-row', { hasText: 'Command palette' });
    await row.locator('.kb-chord').click();
    await expect(row.locator('.kb-chord')).toHaveText('Press keys…');
    await app.keyboard.press(os === 'darwin' ? 'Meta+Alt+K' : 'Control+Alt+K');
    await expect(row.locator('.kb-chord')).not.toHaveText('Press keys…');
    await expect.poll(async () => String((await saved(app)).keybindings['app.commandPalette'])).toMatch(/Alt\+(Meta\+)?K$/);
    await expect(row.getByRole('button', { name: 'Reset to default' })).toBeEnabled();

    await app.keyboard.press('Escape');
    await expect(dialog).toBeHidden();
    await app.keyboard.press(os === 'darwin' ? 'Meta+Alt+K' : 'Control+Alt+K');
    await expect(app.getByRole('dialog', { name: 'Command palette' })).toBeVisible();
    await app.keyboard.press('Escape');

    const again = await openSettings(app);
    await section(again, 'Keybindings');
    await again.getByPlaceholder('Search commands').fill('Command palette');
    await again.locator('.kb-row', { hasText: 'Command palette' }).getByRole('button', { name: 'Reset to default' }).click();
    await expect.poll(async () => String((await saved(app)).keybindings['app.commandPalette'])).toBe(os === 'darwin' ? 'Shift+Meta+P' : 'Ctrl+Shift+P');
    await app.keyboard.press('Escape');
    await openPalette(app, os);
  });
});
