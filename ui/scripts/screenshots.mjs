/**
 * Steps for scripts/shot.mjs that take the README and Microsoft Store screenshots from the mock backend (every host,
 * user, file and number in them is made up: see src/bridge/mock/demoShell.ts and the mock seed data).
 *
 *   node scripts/shot.mjs --name screenshots --steps scripts/screenshots.mjs --out <folder>/final.png \
 *                         --width 1600 --height 960 --scale 1.5
 *
 * Writes main.png, sftp-panel.png, file-manager.png, putty-import.png, session-settings.png and themes.png into the
 * folder of --out (final.png, shot.mjs's own last screenshot, is not needed).
 */
import { dirname, join } from 'node:path';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export default async function screenshots(page, { out }) {
  try {
    await scenes(page, dirname(out));
  } catch (e) {
    await page.screenshot({ path: join(dirname(out), 'failed.png') });  // where it stopped
    throw e;
  }
}

async function scenes(page, dir) {
  const shot = (name) => page.screenshot({ path: join(dir, name) });
  const key = (k) => page.keyboard.press(k);
  const typeInTerminal = async (text) => {
    await page.keyboard.type(text, { delay: 5 });
    await key('Enter');
    await sleep(300);
  };
  const sessionRow = (name) =>
    page.locator('.tree-row').filter({ has: page.locator('.tree-row__name', { hasText: new RegExp(`^${name}$`) }) }).first();
  const connect = async (name) => {
    await sessionRow(name).dblclick();
    await sleep(900);
  };

  // The DOM renderer: headless Edge does not repaint the WebGL terminal reliably after a resize.
  await key('Control+,');
  await sleep(600);
  await page.getByText('Terminal', { exact: true }).first().click();
  await sleep(300);
  await page.getByRole('radio', { name: 'DOM' }).or(page.getByRole('button', { name: 'DOM', exact: true })).first().click();
  await sleep(300);
  await key('Escape');
  await sleep(400);

  // ---- main: three tabs, the first split in two ----------------------------------------------------------------
  await connect('staging');
  await typeInTerminal('docker ps');
  await connect('db-primary');
  await typeInTerminal('df -h');
  await connect('web-01');
  await key('Alt+Shift+=');
  await sleep(1000);
  await typeInTerminal('tail -f /var/log/nginx/access.log');
  await key('Alt+Shift+ArrowLeft');  // back to the first pane, now at its final size
  await sleep(300);
  await typeInTerminal('htop');
  await sleep(500);
  await shot('main.png');

  // ---- SFTP panel beside the staging terminal -------------------------------------------------------------------
  await page.locator('.tab', { hasText: 'staging' }).first().click();
  await sleep(400);
  await typeInTerminal('clear');
  await typeInTerminal('ls -la');
  await key('Control+Shift+S');
  await sleep(1200);
  await shot('sftp-panel.png');
  await key('Control+Shift+S');
  await sleep(400);

  // ---- two-pane file manager -----------------------------------------------------------------------------------
  await key('Control+Shift+P');
  await sleep(300);
  await page.keyboard.type('file manager', { delay: 10 });
  await sleep(300);
  await key('Enter');
  await sleep(1500);
  // A download in the queue: the big backup from the server pane.
  await page.getByText('backup-2026-10-01.tar.gz', { exact: true }).last().click();
  await key('F5');
  await sleep(500);
  const confirm = page.getByRole('dialog').getByRole('button', { name: /^(OK|Copy|Download|Start)/ });
  if (await confirm.count()) await confirm.first().click();
  await sleep(1500);
  await shot('file-manager.png');

  // ---- PuTTY import ------------------------------------------------------------------------------------------
  await page.locator('.tab', { hasText: 'web-01' }).first().click();
  await sleep(300);
  await key('Control+Shift+P');
  await sleep(300);
  await page.keyboard.type('import from putty', { delay: 10 });
  await sleep(300);
  await key('Enter');
  await sleep(1200);
  await shot('putty-import.png');
  await key('Escape');
  await sleep(400);

  // ---- session settings ----------------------------------------------------------------------------------------
  await sessionRow('web-01').click({ button: 'right' });
  await sleep(300);
  await page.locator('[role="menuitem"]', { hasText: /^Edit/ }).first().click();
  await sleep(900);
  await shot('session-settings.png');
  await key('Escape');
  await sleep(400);

  // ---- themes (settings › appearance) ---------------------------------------------------------------------------
  await key('Control+,');
  await sleep(800);
  await page.locator('[role="tab"], button', { hasText: /^Appearance$/ }).first().click();
  await sleep(800);
  await shot('themes.png');
  await key('Escape');
  await sleep(400);

  await page.locator('.tab', { hasText: 'web-01' }).first().click();
  await sleep(500);
}
