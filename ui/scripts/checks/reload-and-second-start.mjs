/**
 * Real-app check of a reloaded window and of a second start with two data folders (Windows only; steps for ../app.mjs):
 *
 *   node scripts/app.mjs --steps scripts/checks/reload-and-second-start.mjs      (needs build/vs/Debug/st_testserver.exe)
 *
 * Review items 11.2 and 11.3 of docs/notes/v1.2.0.md:
 * - a page reload closes the old page's connection and opens exactly one new one (the test server counts them); the tab
 *   comes back (also with "Restore tabs on startup" off, the default) and a toast says so;
 * - with data folder A's window open and data folder B's window on top, `SSHTermaq.exe --data-dir A user@host` opens the
 *   tab in A's window, not in B's.
 * Prints "PASS" / "FAIL" lines and throws at the end when one failed (so app.mjs exits non-zero).
 */
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';

const here = dirname(fileURLToPath(import.meta.url));
const SERVER = join(here, '../../../build/vs/Debug/st_testserver.exe');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function startServer() {
  const p = spawn(SERVER, ['--port', '0'], { stdio: ['pipe', 'pipe', 'inherit'] });
  let buf = '';
  p.stdout.on('data', (d) => (buf += d));
  return {
    proc: p,
    async port() {
      for (let i = 0; i < 100; i++) {
        const m = /LISTENING (\d+)/.exec(buf);
        if (m) return Number(m[1]);
        await sleep(50);
      }
      throw new Error('server did not start');
    },
    async events() {
      buf = '';
      p.stdin.write('events\n');
      await sleep(400);
      return buf.split(/\r?\n/).filter(Boolean);
    }
  };
}

const count = (events, name) => events.filter((e) => e === name).length;
let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) ++failed;
};

async function quickConnect(page, target) {
  await page.getByRole('button', { name: 'New tab options' }).click();
  await page.getByRole('menuitem', { name: /Quick connect/ }).click();
  const dialog = page.getByRole('dialog', { name: 'Quick connect' });
  await dialog.getByRole('textbox').fill(target);
  await dialog.getByRole('button', { name: 'Connect' }).click();
}

async function signIn(page) {
  const key = page.getByRole('button', { name: /Accept & Save/ });
  const pw = page.locator('input[type=password]');
  for (let i = 0; i < 100; i++) {
    if (await key.isVisible().catch(() => false)) await key.click();
    if (await pw.first().isVisible().catch(() => false)) {
      await pw.first().fill('secret');
      await page.keyboard.press('Enter');
      return;
    }
    await sleep(100);
  }
  throw new Error('no password prompt');
}

async function waitShell(page) {
  await page.locator('.xterm-rows').first().getByText('$', { exact: false }).first().waitFor({ timeout: 15000 });
}

const tabs = (page) => page.getByRole('tablist', { name: 'Open tabs' }).getByRole('tab');

export default async function reloadAndSecondStart(page, { exe, dataDir, bridge, platform }) {
  if (platform !== 'win32') throw new Error('Windows only (macOS names its instance socket after the data folder)');
  await bridge('settings:update', { terminal: { renderer: 'dom' } });
  const server = startServer();
  const port = await server.port();

  /* ---- 11.2 ---- */
  await quickConnect(page, `test@127.0.0.1:${port}`);
  await signIn(page);
  await waitShell(page);
  let ev = await server.events();
  check(count(ev, 'connect') === 1 && count(ev, 'disconnect') === 0, `before the reload: 1 connection (${count(ev, 'connect')} connect, ${count(ev, 'disconnect')} disconnect)`);

  await sleep(1000);  // the workspace handover is sent 600 ms after a change
  await page.reload();
  await page.getByRole('tablist', { name: 'Open tabs' }).waitFor();
  await signIn(page);  // the restored tab connects again (no saved password)
  await waitShell(page);
  await sleep(1500);
  ev = await server.events();
  check(count(ev, 'connect') === 2, `after the reload: exactly one new connection (${count(ev, 'connect')} connects in all)`);
  check(count(ev, 'disconnect') === 1, `after the reload: the old connection closed (${count(ev, 'disconnect')} disconnect)`);
  check((await tabs(page).count()) === 1, `after the reload: one tab (${await tabs(page).count()})`);
  check(await page.getByText('The window was reloaded').isVisible(), 'the toast says the window was reloaded');

  /* ---- 11.3 ---- */
  const otherData = mkdtempSync(join(tmpdir(), 'sshtermaq-e2e-b-'));
  const b = spawn(exe, ['--data-dir', otherData, '--no-update-check'], {
    env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: '--remote-debugging-port=9334' },
    stdio: 'ignore'
  });
  let browserB;
  for (let i = 0; i < 60 && !browserB; i++) {
    try {
      browserB = await chromium.connectOverCDP('http://127.0.0.1:9334');
    } catch {
      await sleep(500);
    }
  }
  const pageB = browserB.contexts()[0].pages()[0];
  await pageB.getByRole('tablist', { name: 'Open tabs' }).waitFor({ timeout: 20000 });
  await sleep(1000);  // B's window is now the top one
  const tabsA = await tabs(page).count();
  const tabsB = await tabs(pageB).count();

  const t0 = Date.now();
  const second = spawn(exe, ['--data-dir', dataDir, `test@127.0.0.1:${port}`], { stdio: 'ignore' });
  const code = await new Promise((r) => second.on('exit', r));
  check(code === 0 && Date.now() - t0 < 10000, `the second start handed over and exited (code ${code}, ${Date.now() - t0} ms)`);
  await sleep(2000);
  check((await tabs(page).count()) === tabsA + 1, `data folder A's window opened the tab (${tabsA} -> ${await tabs(page).count()})`);
  check((await tabs(pageB).count()) === tabsB, `data folder B's window did not (${tabsB} -> ${await tabs(pageB).count()})`);

  await browserB.close().catch(() => undefined);
  b.kill();
  server.proc.stdin.end();
  // B's web view may still hold its files for a moment; a leftover temp folder is harmless.
  await new Promise((r) => b.once('exit', r)).catch(() => undefined);
  try {
    rmSync(otherData, { recursive: true, force: true, maxRetries: 10, retryDelay: 300 });
  } catch {
    console.log(`(left ${otherData} behind)`);
  }
  if (failed) throw new Error(`${failed} check(s) failed`);
}
