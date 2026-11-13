import { expect, test } from './fixtures';
import { openPalette, sessionItem, terminalText, useDomRenderer } from './helpers';
import type { Page } from '@playwright/test';

/**
 * Names after a restart. The mock backend keeps settings and the saved workspace in localStorage, so reloading the
 * page plays quitting and starting the app again.
 */
async function setBehavior(app: Page, behavior: { restoreTabs: boolean; restoreNames: boolean }): Promise<void> {
  await app.evaluate(async (b) => {
    const module = '/src/bridge/api.ts';
    const { api } = (await import(module)) as typeof import('../src/bridge/api');
    await api.settings.update({ behavior: b });
  }, behavior);
}

/** Whether the workspace saved in the mock's storage holds all of `names`. */
function savedWith(app: Page, ...names: string[]): Promise<boolean> {
  return app.evaluate((list) => {
    const ws = JSON.parse(localStorage.getItem('sshtermaq.mock.v1') ?? '{}')?.state?.workspace;
    const json = JSON.stringify(ws ?? {});
    return list.every((n) => json.includes(JSON.stringify(n)));
  }, names);
}

/** Opens web-01, splits it, names the tab "Release box", then the right pane "logs". */
async function nameTabAndPane(app: Page, os: 'win32' | 'darwin'): Promise<void> {
  await sessionItem(app, 'web-01').dblclick();
  await expect(terminalText(app)).toContainText('deploy@web-01');
  const palette = await openPalette(app, os);
  await app.keyboard.type('split right');
  await palette.getByRole('option', { name: /Split pane right/ }).click();
  const titles = app.locator('.pane-head__title');
  await expect(titles).toHaveCount(2);
  await app.getByRole('tab').first().dblclick();
  const tabField = app.getByRole('textbox', { name: 'Tab title' });
  await tabField.fill('Release box');
  await tabField.press('Enter');
  await expect(app.getByRole('tab', { name: /Release box/ })).toBeVisible();
  // The pane last, once the tab's save is done: a pane's name alone must be saved too (it changes nothing in the
  // layout).
  await expect.poll(() => savedWith(app, 'Release box'), { timeout: 5000 }).toBe(true);
  await titles.last().dblclick();
  const paneField = app.getByRole('textbox', { name: 'Pane name' });
  await paneField.fill('logs');
  await paneField.press('Enter');
  await expect(app.locator('.pane-head__title', { hasText: 'logs' })).toBeVisible();
}

/** Quits and starts again: waits for the debounced save, then reloads. */
async function restart(app: Page): Promise<void> {
  await expect.poll(() => savedWith(app, 'Release box', 'logs'), { timeout: 5000 }).toBe(true);
  await app.reload();
  await expect(app.getByRole('tree', { name: 'Sessions' })).toBeVisible();
}

test.describe('names after a restart', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
  });

  test('with "Keep tab and pane names" on (the default), the tab and the pane keep their names', async ({ app, os }) => {
    await setBehavior(app, { restoreTabs: true, restoreNames: true });
    await nameTabAndPane(app, os);
    await restart(app);
    await expect(app.getByRole('tab', { name: /Release box/ })).toBeVisible();
    await expect(app.locator('.pane-head__title', { hasText: 'logs' })).toBeVisible();
  });

  test('with it off, the tabs come back with automatic titles', async ({ app, os }) => {
    await setBehavior(app, { restoreTabs: true, restoreNames: false });
    await nameTabAndPane(app, os);
    await restart(app);
    await expect(app.getByRole('tab', { name: /web-01/ })).toBeVisible();
    await expect(app.getByRole('tab', { name: /Release box/ })).toHaveCount(0);
    await expect(app.locator('.pane-head__title')).toHaveCount(2);
    await expect(app.locator('.pane-head__title', { hasText: 'logs' })).toHaveCount(0);
  });
});
