import { expect, test } from './fixtures';
import { sessionItem } from './helpers';

test.describe('file manager: download as / upload as', () => {
  test('"Download as…" asks for the name and the transfer uses it', async ({ app }) => {
    await sessionItem(app, 'web-01').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Open file manager' }).click();
    const file = app.getByText('report.pdf', { exact: true }).last();
    await expect(file).toBeVisible();
    await file.click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Download as…' }).click();

    const dialog = app.getByRole('dialog');
    const input = dialog.getByRole('textbox');
    await expect(input).toHaveValue('report.pdf');
    await input.fill('a/b.pdf');
    await expect(dialog).toContainText('cannot contain');
    await input.fill('report-2026.pdf');
    await dialog.getByRole('button', { name: 'Download' }).click();

    await expect(app.getByText('report-2026.pdf').first()).toBeVisible();
  });

  test('"Upload as…" is offered for an item of this computer', async ({ app, os }) => {
    await sessionItem(app, 'web-01').click({ button: 'right' });
    await app.getByRole('menuitem', { name: 'Open file manager' }).click();
    // The left pane (this computer) starts in Documents on Windows, in the home folder on macOS.
    const localPane = app.getByRole('grid').first();
    const file = localPane.getByText(os === 'darwin' ? 'Documents' : 'notes.txt', { exact: true });
    await expect(file).toBeVisible();
    await file.click({ button: 'right' });
    await expect(app.getByRole('menuitem', { name: 'Upload as…' })).toBeEnabled();
  });
});
