import type { Page } from '@playwright/test';
import { expect, test } from './fixtures';

/** What the host sends when it was started with arguments: a clicked link (`fromLink`) or a plain command line. */
async function openRequest(page: Page, fromLink: boolean, proxyCommand = ''): Promise<void> {
  await page.evaluate(
    async ({ fromLink, proxyCommand }) => {
      const module = '/src/shared/defaults.ts';
      const { createDefaultSession } = (await import(module)) as typeof import('../src/shared/defaults');
      const session = createDefaultSession({ host: 'links.example', port: 22, username: 'deploy' });
      if (proxyCommand) session.proxy = { ...session.proxy, type: 'command', localCommand: proxyCommand };
      const emit = (globalThis as unknown as { __mockEmit(ch: string, ...a: unknown[]): void }).__mockEmit;
      emit('app:onOpenRequest', { session, ...(fromLink ? { fromLink: true } : {}) });
    },
    { fromLink, proxyCommand }
  );
}

test.describe('links (ssh://, sftp://, ...)', () => {
  test('a link asks before it connects and shows the proxy command; cancelling opens nothing', async ({ app }) => {
    await openRequest(app, true, 'nc -X connect -x proxy:8080 %host %port');
    const dialog = app.getByRole('dialog', { name: 'Open a link?' });
    await expect(dialog).toContainText('Open an SSH connection to deploy@links.example:22?');
    await expect(dialog).toContainText('It first runs this command on this computer');
    await expect(dialog.locator('.dlg-confirm__details')).toHaveText('nc -X connect -x proxy:8080 %host %port');
    await dialog.getByRole('button', { name: 'Cancel' }).click();
    await expect(dialog).toBeHidden();
    await expect(app.getByRole('tab', { name: /links\.example/ })).toHaveCount(0);

    await openRequest(app, true);
    await expect(dialog).toBeVisible();
    await expect(dialog.locator('.dlg-confirm__details')).toHaveCount(0);  // no proxy command, nothing to show
    await dialog.getByRole('button', { name: 'Connect' }).click();
    await expect(app.getByRole('tab', { name: /links\.example/ })).toBeVisible();
  });

  test('a plain command line connects without asking', async ({ app }) => {
    await openRequest(app, false, 'nc %host %port');
    await expect(app.getByRole('tab', { name: /links\.example/ })).toBeVisible();
    await expect(app.getByRole('dialog', { name: 'Open a link?' })).toHaveCount(0);
  });

  test('"Don\'t ask again" turns the question off, and the setting turns it back on', async ({ app }) => {
    await openRequest(app, true);
    const dialog = app.getByRole('dialog', { name: 'Open a link?' });
    await dialog.getByText("Don't ask again for links").click();
    await dialog.getByRole('button', { name: 'Connect' }).click();
    await expect(app.getByRole('tab', { name: /links\.example/ })).toHaveCount(1);

    await openRequest(app, true);  // no question now
    await expect(app.getByRole('tab', { name: /links\.example/ })).toHaveCount(2);
    await expect(dialog).toHaveCount(0);

    await app.getByRole('complementary', { name: 'Saved sessions' }).getByRole('button', { name: 'Settings' }).click();
    const settings = app.getByRole('dialog', { name: 'Settings' });
    await settings.getByRole('navigation', { name: 'Settings sections' }).getByRole('button', { name: 'Behavior' }).click();
    const row = settings.locator('.dlg-row', { has: app.getByText('Ask before opening links', { exact: true }) });
    await expect(row.getByRole('switch')).not.toBeChecked();
    await row.locator('label.switch').click();
    await expect(row.getByRole('switch')).toBeChecked();
    await app.keyboard.press('Escape');
    await expect(settings).toBeHidden();

    await openRequest(app, true);  // asks again
    await expect(dialog).toBeVisible();
  });
});
