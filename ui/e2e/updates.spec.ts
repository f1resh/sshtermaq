import { expect, test } from './fixtures';
import type { Page } from '@playwright/test';

/**
 * The version check (1.2.0 item 12) against the mock backend (src/bridge/mock/core.ts, `update:check`): the mock runs
 * 1.2.0 and by default its latest release is 1.2.0, so other tests never see the notice. Here each test sets
 * globalThis.__mockUpdates before the page loads. The first check runs 5 s after start.
 */
interface MockUpdates {
  latest?: string;
  error?: string;
  channel?: 'store' | 'homebrew' | 'other';
  requests?: number;
}

async function load(app: Page, m: MockUpdates): Promise<void> {
  await app.addInitScript((mock) => {
    (globalThis as { __mockUpdates?: MockUpdates }).__mockUpdates = mock;
    // Links open through window.open in the mock: keep them here instead of loading GitHub.
    window.open = ((url: string) => {
      (globalThis as { __opened?: string[] }).__opened = [...((globalThis as { __opened?: string[] }).__opened ?? []), url];
      return null;
    }) as typeof window.open;
  }, m);
  await app.reload();
  await expect(app.getByRole('tree', { name: 'Sessions' })).toBeVisible({ timeout: 15_000 });
}

const chip = (app: Page) => app.getByRole('button', { name: /^SSHTermaq .* is available/ });
const requests = (app: Page) => app.evaluate(() => (globalThis as { __mockUpdates?: MockUpdates }).__mockUpdates?.requests ?? 0);
const opened = (app: Page) => app.evaluate(() => (globalThis as { __opened?: string[] }).__opened ?? []);
const settingsButton = (app: Page) => app.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: /^Settings/ });

async function openAbout(app: Page) {
  await app.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: /^Settings/ }).click();
  const dialog = app.getByRole('dialog', { name: 'Settings' });
  await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: /^About/ }).click();
  return dialog.getByRole('region', { name: 'Updates' });
}

test.describe('version check', () => {
  test('a newer release: a chip next to the window buttons, its menu, and a dot on Settings and About', async ({ app, os }) => {
    await load(app, { latest: '1.3.0' });
    await expect(chip(app)).toBeVisible({ timeout: 10_000 });
    await expect(chip(app)).toHaveAccessibleName('SSHTermaq 1.3.0 is available (you have 1.2.0)');
    // Windows: just before the caption buttons; macOS: right after the traffic lights.
    if (os === 'win32') await expect(app.locator('.titlebar__update + .caption')).toHaveCount(1);
    else await expect(app.locator('.titlebar__lights + .titlebar__update')).toHaveCount(1);

    await chip(app).click();
    await expect(app.getByText('Version 1.3.0 is available', { exact: true })).toBeVisible();
    await expect(app.getByRole('menuitem', { name: /brew upgrade/ })).toHaveCount(0);  // not a Homebrew copy
    await app.getByRole('menuitem', { name: 'Open release page' }).click();
    await expect.poll(() => opened(app)).toEqual(['https://github.com/f1resh/sshtermaq/releases/tag/1.3.0']);

    // The dot on Settings opens Settings at About, where the About entry has its dot too.
    await expect(settingsButton(app).locator('.update-dot')).toHaveCount(1);
    await settingsButton(app).click();
    const dialog = app.getByRole('dialog', { name: 'Settings' });
    await expect(dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: /^About/ }).locator('.update-dot')).toHaveCount(1);
    const updates = dialog.getByRole('region', { name: 'Updates' });
    await expect(updates).toContainText('SSHTermaq 1.3.0 is available (you have 1.2.0).');
    await expect(updates.getByRole('button', { name: 'Open release page' })).toBeVisible();
    expect(await requests(app)).toBe(1);
  });

  test('"Remind me tomorrow" hides it, also after a restart; "Check now" brings it back', async ({ app }) => {
    await load(app, { latest: '1.3.0' });
    await chip(app).click({ timeout: 10_000 });
    await app.getByRole('menuitem', { name: 'Remind me tomorrow' }).click();
    await expect(chip(app)).toHaveCount(0);
    await expect(settingsButton(app).locator('.update-dot')).toHaveCount(0);

    await load(app, { latest: '1.3.0' });  // a restart the same day: no new request, still hidden
    await app.waitForTimeout(6000);
    await expect(chip(app)).toHaveCount(0);
    expect(await requests(app)).toBe(0);

    const updates = await openAbout(app);
    await updates.getByRole('button', { name: 'Check now' }).click();
    await expect(chip(app)).toBeVisible();
  });

  test('"Skip this version": gone; About still says it is out; a later version is offered', async ({ app }) => {
    await load(app, { latest: '1.3.0' });
    await chip(app).click({ timeout: 10_000 });
    await app.getByRole('menuitem', { name: 'Skip this version' }).click();
    await expect(chip(app)).toHaveCount(0);
    const updates = await openAbout(app);
    await expect(updates).toContainText('You chose to skip this version.');
    await expect(updates.getByRole('button', { name: 'Skip this version' })).toHaveCount(0);
    await app.evaluate(() => void ((globalThis as { __mockUpdates?: MockUpdates }).__mockUpdates!.latest = '1.3.1'));
    await updates.getByRole('button', { name: 'Check now' }).click();
    await expect(chip(app)).toHaveAccessibleName('SSHTermaq 1.3.1 is available (you have 1.2.0)');
  });

  test('up to date: nothing in the title bar; About says so; the setting off means no request', async ({ app }) => {
    await load(app, {});
    await expect.poll(() => requests(app), { timeout: 10_000 }).toBe(1);
    await expect(chip(app)).toHaveCount(0);
    const updates = await openAbout(app);
    await expect(updates).toContainText('You have the latest version.');
    const toggle = updates.getByRole('switch');  // "Check for new versions"
    await expect(toggle).toBeChecked();
    await updates.locator('label.switch').click();
    await expect(toggle).not.toBeChecked();

    // Off, a day later (the saved check moved back): no request at start.
    await app.evaluate(async () => {
      const module = '/src/bridge/api.ts';
      const { api } = (await import(module)) as typeof import('../src/bridge/api');
      await api.app.setState('updateCheck', { lastCheck: Date.now() - 2 * 24 * 3600 * 1000 });
    });
    await load(app, {});
    await app.waitForTimeout(6000);
    expect(await requests(app)).toBe(0);
  });

  test('a failed check says why in About, and nothing shows in the title bar', async ({ app }) => {
    await load(app, { error: 'the server answered 403' });
    await expect.poll(() => requests(app), { timeout: 10_000 }).toBe(1);
    await expect(chip(app)).toHaveCount(0);
    const updates = await openAbout(app);
    await expect(updates).toContainText('The check failed: the server answered 403');
  });

  test('Homebrew: the menu offers the upgrade command', async ({ app }) => {
    await load(app, { latest: '1.3.0', channel: 'homebrew' });
    await chip(app).click({ timeout: 10_000 });
    await app.getByRole('menuitem', { name: 'Copy “brew upgrade --cask sshtermaq”' }).click();
    await expect.poll(() => app.evaluate(() => (globalThis as { __mockClipboard?: string }).__mockClipboard)).toBe('brew upgrade --cask sshtermaq');
  });

  test('a Store package: no request, no chip, no Updates section', async ({ app }) => {
    await load(app, { latest: '1.3.0', channel: 'store' });
    await app.waitForTimeout(6000);
    expect(await requests(app)).toBe(0);
    await expect(chip(app)).toHaveCount(0);
    await app.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: /^Settings/ }).click();
    const dialog = app.getByRole('dialog', { name: 'Settings' });
    await dialog.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: /^About/ }).click();
    await expect(dialog.getByText('Third-party software')).toBeVisible();
    await expect(dialog.getByRole('region', { name: 'Updates' })).toHaveCount(0);
  });
});
