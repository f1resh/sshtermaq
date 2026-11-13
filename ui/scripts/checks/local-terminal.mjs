/**
 * Real-app check of local terminals, the same on Windows and macOS (steps for ../app.mjs):
 *
 *   node scripts/app.mjs --steps scripts/checks/local-terminal.mjs            (Windows: build\vs\Debug\SSHTermaq.exe)
 *   node scripts/app.mjs --steps scripts/checks/local-terminal.mjs            (macOS: the Debug build in build/mac-*)
 *
 * Opens the default shell with the shortcut, runs commands (environment, UTF-8, the window size), checks that `exit`
 * closes the tab, opens a second shell from the + menu and, on Windows, a WSL distribution if there is one. Prints "PASS" / "FAIL" lines
 * and throws on the first failure (so app.mjs exits non-zero). Screenshots go next to --out.
 */
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export default async function localTerminalCheck(page, { bridge, platform, out }) {
  const mac = platform === 'darwin';
  const shot = (name) => page.screenshot({ path: out.replace(/\.png$/, `-${name}.png`) });
  const screen = () => page.evaluate(() => [...document.querySelectorAll('.xterm-rows')].map((r) => r.innerText).join('\n----\n'));
  const tabs = () => page.evaluate(() => document.querySelectorAll('.tabstrip .tab').length);
  const pass = (what) => console.log(`PASS  ${what}`);
  const fail = async (what) => {
    await shot('failed');
    console.log(`FAIL  ${what}\n--- screen ---\n${await screen()}`);
    throw new Error(what);
  };
  async function waitFor(what, test, ms = 20000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
      if (await test()) return pass(what);
      await sleep(250);
    }
    await fail(what);
  }
  const showing = (text) => async () => (await screen()).includes(text);
  async function typeLine(text) {
    await page.keyboard.type(text);
    await page.keyboard.press('Enter');
  }
  /** Clicks a menu item by its text (both drivers: a DOM click). */
  const clickMenuItem = (label) =>
    page.evaluate((l) => {
      const item = [...document.querySelectorAll('[role="menuitem"]')].find((e) => e.textContent.trim().startsWith(l));
      item?.click();
      return !!item;
    }, label);

  await bridge('settings:update', { terminal: { renderer: 'dom' } });
  await sleep(1000);

  // The shells of this computer: at least one, exactly one default.
  const shells = await bridge('shells:list');
  console.log(`shells: ${shells.map((s) => `${s.name}${s.isDefault ? ' (default)' : ''}`).join(', ')}`);
  if (!shells.length) await fail('the computer has shells');
  if (shells.filter((s) => s.isDefault).length !== 1) await fail('exactly one default shell');
  pass(`${shells.length} shells found`);
  const def = shells.find((s) => s.isDefault);
  const powershell = /powershell|pwsh/i.test(def.command);
  const cmdShell = /cmd\.exe/i.test(def.command) && !powershell;

  // 1. The shortcut opens the default shell.
  await page.keyboard.press(mac ? 'Meta+Shift+T' : 'Control+Shift+Backquote');
  await waitFor('the shortcut opens a local terminal', async () => (await tabs()) === 1);
  await waitFor('the shell is connected', () =>
    page.evaluate(() => document.querySelector('.term-pane')?.getAttribute('data-status') === 'connected')
  );
  await sleep(1500); // the first prompt

  // 2. Environment, UTF-8 and the window size, in the default shell's own syntax.
  const env = powershell ? '"[$env:TERM_PROGRAM $env:COLORTERM]"' : cmdShell ? '[%TERM_PROGRAM% %COLORTERM%]' : '"[$TERM_PROGRAM $COLORTERM]"';
  await typeLine(`echo ${env}`);
  await waitFor('TERM_PROGRAM and COLORTERM are set', showing('[SSHTermaq truecolor]'));
  await typeLine(powershell ? 'echo "Привет-$(6*7)"' : cmdShell ? 'echo Привет-42' : 'echo "Привет-$((6*7))"');
  await waitFor('non-ASCII text goes both ways', showing('Привет-42'));
  if (!powershell && !cmdShell) {
    await typeLine('echo "size:$(tput cols)x$(tput lines)"');
    await waitFor('the shell sees the window size', async () => /size:\d{2,}x\d{2,}/.test(await screen()));
    if (mac) {
      await typeLine('echo "lang:$LANG"');
      await waitFor('LANG is set (an app started from the Finder gets none)', async () => /lang:\w+\.UTF-8/.test(await screen()));
    }
  }
  await shot('default-shell');

  // 3. exit closes the tab (exit code 0, "close on clean exit").
  await typeLine('exit');
  await waitFor('exit closes the tab', async () => (await tabs()) === 0, 10000);

  // 4. Another shell from the + menu.
  const other = shells.find((s) => !s.isDefault) ?? def;
  await page.click('.newtab__btn--menu');
  await sleep(400);
  if (!(await clickMenuItem(other.name))) await fail(`the + menu lists ${other.name}`);
  await waitFor(`${other.name} opens from the + menu`, () =>
    page.evaluate(() => document.querySelector('.term-pane')?.getAttribute('data-status') === 'connected')
  );
  await sleep(1500);
  await shot('second-shell');

  // 5. Windows: a WSL distribution, if there is one, really starts (wsl.exe is picky about its command line).
  const wsl = shells.find((s) => s.id.startsWith('wsl:'));
  if (wsl) {
    await page.click('.newtab__btn--menu');
    await sleep(400);
    if (!(await clickMenuItem(wsl.name))) await fail(`the + menu lists ${wsl.name}`);
    await waitFor(`${wsl.name} shows a prompt`, async () => {
      const text = await screen();
      if (/DISTRO_NOT_FOUND|no distribution with the supplied name/i.test(text)) await fail(`WSL refused the distribution name (${wsl.command})`);
      return /[$#]\s*$/m.test(text);
    }, 30000);
    await typeLine('echo "wsl-$((6*7)):$(uname -s)"');
    await waitFor(`${wsl.name} runs commands`, showing('wsl-42:Linux'));
    await shot('wsl');
  }
  console.log('done: every check passed');
}
