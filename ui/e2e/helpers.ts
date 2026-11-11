/**
 * Small actions shared by the specs. They find things the way a user does (roles, labels, visible text), so the
 * tests don't depend on CSS class names.
 */
import { expect, type Locator, type Page } from '@playwright/test';

const escapeRegExp = (s: string) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/**
 * A saved session in the sidebar, by its name. Its row reads "<name> <user@host>", so "web-01" must be followed by a
 * space and not by "(copy)".
 */
export const sessionItem = (page: Page, name: string): Locator =>
  page.getByRole('tree', { name: 'Sessions' }).getByRole('treeitem', { name: new RegExp(`^${escapeRegExp(name)}( [^(]|$)`) });

/** Opens the session editor of a saved session through its context menu. */
export async function editSession(page: Page, name: string): Promise<Locator> {
  await sessionItem(page, name).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Edit…' }).click();
  await expect(page.getByRole('dialog', { name: `Edit session ${name}` })).toBeVisible();
  return page.getByRole('dialog', { name: /^Edit session / });   // the title follows the Name field as it is edited
}

/** Shows one page of the session editor ("SSH", "Authentication", "Proxy", ...). */
export async function editorPage(dialog: Locator, category: string): Promise<void> {
  await dialog.getByRole('navigation', { name: 'Session categories' }).getByRole('button', { name: category, exact: true }).click();
  await expect(dialog.getByRole('heading', { level: 2, name: category })).toBeVisible();
}

/**
 * Switches the terminals to xterm's DOM renderer, whose rows are plain text in the page (`.xterm-rows`); the default
 * WebGL renderer draws on a canvas. Goes through the app's own bridge module, as the settings dialog would.
 */
export async function useDomRenderer(page: Page): Promise<void> {
  await page.evaluate(async () => {
    const module = '/src/bridge/api.ts';   // served by Vite in the page; a variable, so tsc doesn't look for it
    const { api } = (await import(module)) as typeof import('../src/bridge/api');
    await api.settings.update({ terminal: { renderer: 'dom' } });
  });
}

/** Answers a "Close tab?" / "Close pane?" question with Close, if one is shown. */
export async function confirmClose(page: Page): Promise<void> {
  const dialog = page.getByRole('dialog', { name: /^Close .*\?$/ });
  if (await dialog.isVisible().catch(() => false)) await dialog.getByRole('button', { name: 'Close', exact: true }).last().click();
  await expect(dialog).toBeHidden();
}

/**
 * Opens the command palette the way the system does: its shortcut on Windows; on macOS the shortcut belongs to the
 * native menu bar, whose command the test sends as the host would (window:onMenuCommand, see src/bridge/mock).
 */
export async function openPalette(page: Page, os: 'win32' | 'darwin'): Promise<Locator> {
  if (os === 'darwin')
    await page.evaluate(() => (globalThis as unknown as { __mockEmit(ch: string, ...a: unknown[]): void }).__mockEmit('window:onMenuCommand', 'app.commandPalette'));
  else await page.keyboard.press('Control+Shift+P');
  const palette = page.getByRole('dialog', { name: 'Command palette' });
  await expect(palette).toBeVisible();
  return palette;
}

/** The text of the first terminal on the page (needs useDomRenderer). */
export const terminalText = (page: Page): Locator => page.locator('.xterm-rows').first();
