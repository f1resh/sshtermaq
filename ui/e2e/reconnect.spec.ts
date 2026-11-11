import { expect, test } from './fixtures';
import { dropConnections, sessionItem, terminalText, useDomRenderer } from './helpers';

test.describe('connection lost and restarted', () => {
  test.beforeEach(async ({ app }) => {
    await useDomRenderer(app);
    await sessionItem(app, 'web-01').dblclick();
    await expect(terminalText(app)).toContainText('deploy@web-01');
  });

  test('the new session starts below the old output instead of over it', async ({ app }) => {
    await app.keyboard.type('help');
    await app.keyboard.press('Enter');
    await expect(terminalText(app)).toContainText('Commands: help');

    await dropConnections(app);
    await app.getByRole('button', { name: 'Restart' }).click();
    await expect.poll(async () => (await terminalText(app).innerText()).split('Welcome to Ubuntu').length - 1).toBe(2);

    // The old output is still there, whole, and the new login follows it after an empty line.
    const lines = (await terminalText(app).locator(':scope > div').allInnerTexts()).map((l) => l.trimEnd());
    const help = lines.findIndex((l) => l.startsWith('Commands: help'));
    const second = lines.findIndex((l, i) => i > help && l.startsWith('Welcome to Ubuntu'));
    expect(help).toBeGreaterThan(0);
    expect(lines[help - 1]).toMatch(/deploy@web-01:~\$ help$/);
    expect(second).toBeGreaterThan(help);
    expect(lines.slice(help + 1, second).some((l) => l === '')).toBe(true);
  });

  test('the SFTP panel stays in its folder', async ({ app }) => {
    await app.getByRole('contentinfo').getByRole('button', { name: 'SFTP', exact: true }).click();
    const path = app.getByRole('textbox', { name: 'Path' });
    await expect(path).toHaveValue('/home/demo');
    await app.getByTitle('/home/demo/logs', { exact: true }).dblclick();
    await expect(path).toHaveValue('/home/demo/logs');

    await dropConnections(app);
    await expect(app.getByText('Connect the session to browse its files.')).toBeVisible();
    await app.getByRole('button', { name: 'Restart' }).click();
    await expect(path).toHaveValue('/home/demo/logs');
    await expect(app.getByTitle('/home/demo/logs/', { exact: false }).first()).toBeVisible();
  });
});
