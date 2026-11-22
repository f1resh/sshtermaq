/**
 * A server whose host key changed (review 8): the question says what the connection will hold back; accepted, the
 * terminal says so and the session's tunnels wait for Special Commands > Open this session's tunnels. The mock's
 * CHANGED_KEY_HOST plays that server; any other host connects as before, without the question.
 */
import { expect, test } from './fixtures';
import { sessionItem, terminalText, useDomRenderer } from './helpers';
import type { Page } from '@playwright/test';

const HOST = 'changed-key.example.test';

async function quickConnect(app: Page, target: string) {
  await app.getByRole('button', { name: 'New tab options' }).click();
  await app.getByRole('menuitem', { name: /Quick connect/ }).click();
  const dialog = app.getByRole('dialog', { name: 'Quick connect' });
  await dialog.getByRole('textbox').fill(target);
  await dialog.getByRole('button', { name: 'Connect' }).click();
}

async function specialCommands(app: Page) {
  await app.locator('.xterm').first().click({ button: 'right', modifiers: ['Shift'] });  // a plain right-click pastes
  await app.getByRole('menuitem', { name: 'Special Commands' }).hover();
}

test.describe('a changed host key', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
  });

  test('connect once: no forwarding, tunnels when asked for', async ({ app }) => {
    await quickConnect(app, `deploy@${HOST}`);
    const question = app.getByRole('dialog', { name: 'Host key changed: potential security breach' });
    await expect(question).toBeVisible();
    await expect(question.getByTestId('hostkey-lockdown')).toContainText('does not send a saved password');
    await expect(question.getByTestId('hostkey-lockdown')).toContainText('tunnels stay off');
    await question.getByRole('button', { name: 'Connect once' }).click();

    await expect(terminalText(app)).toContainText("The server's host key changed");
    await specialCommands(app);
    await app.getByRole('menuitem', { name: "Open this session's tunnels" }).click();

    // Asked once: the command is gone from the list, the others stay.
    await specialCommands(app);
    await expect(app.getByRole('menuitem', { name: 'Break' })).toBeVisible();
    await expect(app.getByRole('menuitem', { name: "Open this session's tunnels" })).toHaveCount(0);
  });

  test('cancel ends the connection', async ({ app }) => {
    await quickConnect(app, `deploy@${HOST}`);
    const question = app.getByRole('dialog', { name: 'Host key changed: potential security breach' });
    await question.getByRole('button', { name: 'Cancel' }).click();
    await expect(question).toBeHidden();
    await expect(terminalText(app)).not.toContainText("The server's host key changed");
  });

  test('a known host connects as before: no question, no held tunnels', async ({ app }) => {
    await sessionItem(app, 'web-01').dblclick();
    await expect(terminalText(app)).toContainText('deploy@web-01');
    await expect(app.getByRole('dialog', { name: /Host key/ })).toHaveCount(0);
    await specialCommands(app);
    await expect(app.getByRole('menuitem', { name: 'Break' })).toBeVisible();
    await expect(app.getByRole('menuitem', { name: "Open this session's tunnels" })).toHaveCount(0);
  });
});
