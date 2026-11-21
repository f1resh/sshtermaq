/**
 * Real-app check of terminal fonts with both renderers, the same on Windows and macOS (steps for ../app.mjs):
 *
 *   node scripts/app.mjs --steps scripts/checks/terminal-fonts.mjs --out ../artifacts/fonts.png
 *
 * For a system font (Menlo / Consolas) and, if one is installed, a Nerd Font, with the Renderer setting on GPU (WebGL)
 * and on DOM: prints a line of text, powerline and Nerd Font icons, box drawing and blocks in a local terminal, checks
 * which renderer the pane really uses and saves a screenshot per case (fonts-<font>-<renderer>.png next to --out).
 *
 * Checked here: with the WebGL setting the pane uses WebGL exactly when a canvas draws the font as the page does
 * (measured again here, apart from the app's own check); with the DOM setting it uses DOM; on Windows a Nerd Font stays
 * on WebGL; when a font falls back to DOM, Settings › Terminal › Renderer says so. Checked by eye in the screenshots:
 * every case shows the font's own letters, joined box lines and the powerline arrows (xterm draws those itself); the
 * Nerd Font shows its icons as pictures with both settings (no boxes or blanks), while the system font shows boxes for
 * the icons it doesn't have.
 * Prints PASS / FAIL lines and throws on the first failure. The Nerd Font part is skipped when none is installed.
 */
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** The test line: text, powerline (E0B0 E0B2 E0B6), Nerd Font icons (E7B5 F113 F8FF E60B, F0A1E beyond the BMP), box drawing, blocks. */
const LINE = [...'Aa0 '].map((c) => c.codePointAt(0)).concat(
  0xe0b0, 0xe0b2, 0x20, 0xe0b6, 0x20, 0x7c, 0x20, 0xe7b5, 0x20, 0xf113, 0x20, 0xf8ff, 0x20, 0xe60b, 0x20, 0xf0a1e, 0x20, 0x7c, 0x20,
  0x2500, 0x253c, 0x2500, 0x20, 0x256d, 0x256e, 0x20, 0x2588, 0x2593, 0x2591, 0x20, 0x7c, 0x20, 0x6f, 0x6b
);

/** A command that prints LINE in the shell: printf with octal UTF-8 bytes (any POSIX shell) or PowerShell. */
function printCommand(shellCommand) {
  if (/powershell|pwsh/i.test(shellCommand)) {
    return `cls; Write-Host ([string]::Concat((@(${LINE.map((c) => '0x' + c.toString(16)).join(',')}) | % { [char]::ConvertFromUtf32($_) })))`;
  }
  if (/cmd\.exe/i.test(shellCommand)) return null;
  const octal = [...Buffer.from(String.fromCodePoint(...LINE), 'utf8')].map((b) => '\\' + b.toString(8).padStart(3, '0')).join('');
  return `clear; printf '${octal}\\n'`;
}

export default async function terminalFontsCheck(page, { bridge, platform, out }) {
  const mac = platform === 'darwin';
  const shot = (name) => page.screenshot({ path: out.replace(/\.png$/, `-${name}.png`) });
  const pass = (what) => console.log(`PASS  ${what}`);
  const fail = async (what) => {
    await shot('failed');
    console.log(`FAIL  ${what}`);
    throw new Error(what);
  };
  const canvases = () => page.evaluate(() => document.querySelectorAll('.xterm canvas').length);
  /** The page's own measurement: does a canvas draw this font list as page text does? */
  const canvasMatchesPage = (family) =>
    page.evaluate((f) => {
      const text = 'mmmmmiiiiiWWWWWlllll@@@@@00000';
      const font = `64px ${f}`;
      const ctx = document.createElement('canvas').getContext('2d');
      ctx.font = font;
      const span = document.createElement('span');
      span.textContent = text;
      span.style.cssText = 'position:absolute;left:-10000px;white-space:pre;letter-spacing:0;font-kerning:none';
      span.style.font = font;
      document.body.appendChild(span);
      const dom = span.getBoundingClientRect().width;
      span.remove();
      return { canvas: Math.round(ctx.measureText(text).width * 10) / 10, dom: Math.round(dom * 10) / 10 };
    }, family);
  const rendererNote = async () => {
    await page.keyboard.press(mac ? 'Meta+,' : 'Control+,');
    await sleep(1200);
    await page.evaluate(() => [...document.querySelectorAll('button, [role="tab"], [role="option"], a, li')].find((e) => e.textContent.trim() === 'Terminal')?.click());
    await sleep(800);
    const text = await page.evaluate(() => {
      const label = [...document.querySelectorAll('*')].find((e) => e.children.length === 0 && e.textContent.trim() === 'Renderer');
      return label?.parentElement?.parentElement?.textContent ?? '';
    });
    await page.keyboard.press('Escape');
    await sleep(600);
    return text;
  };

  // Fonts: the system's monospace font and the first Nerd Font installed, if any.
  const installed = (await bridge('app:listFonts')) ?? [];
  const nerd = installed.find((f) => /Nerd Font|\bNFM?\b/.test(f.family));
  const fonts = [mac ? 'Menlo' : 'Consolas', ...(nerd ? [nerd.family] : [])];
  console.log(`fonts: ${fonts.join(', ')}${nerd ? '' : '  (no Nerd Font installed: that part is skipped; install one, e.g. CaskaydiaMono Nerd Font)'}`);

  // One local terminal for every case.
  const shells = await bridge('shells:list');
  const shell = shells.find((s) => s.isDefault) ?? shells[0];
  const print = shell && printCommand(shell.command);
  if (!print) await fail(`a shell that can print the test line (default: ${shell?.name ?? 'none'}; cmd.exe can't)`);
  await bridge('settings:update', { terminal: { fontFamily: fonts[0], fontSize: 18, renderer: 'webgl' } });
  await sleep(800);
  await page.keyboard.press(mac ? 'Meta+Shift+T' : 'Control+Shift+Backquote');
  await sleep(3000);

  for (const font of fonts) {
    const family = `'${font}', monospace`;
    const m = await canvasMatchesPage(family);
    const drawable = Math.abs(m.canvas - m.dom) <= 0.5;
    console.log(`${font}: canvas ${m.canvas}px, page ${m.dom}px -> ${drawable ? 'a canvas draws it' : 'a canvas falls back to another font'}`);
    if (!mac && !drawable) await fail(`${font}: WebView2 gives canvases the installed fonts`);
    for (const renderer of ['webgl', 'dom']) {
      await bridge('settings:update', { terminal: { fontFamily: family, renderer } });
      await sleep(1200);
      await page.keyboard.type(print);
      await page.keyboard.press('Enter');
      await sleep(1500);
      const name = `${font.replace(/\W+/g, '')}-${renderer}`;
      await shot(`fonts-${name}`);
      const used = (await canvases()) > 0 ? 'webgl' : 'dom';
      const expected = renderer === 'webgl' && drawable ? 'webgl' : 'dom';
      if (used !== expected) await fail(`${font}, setting ${renderer}: the pane uses ${used}, expected ${expected}`);
      pass(`${font}, setting ${renderer}: the pane uses ${used} (screenshot fonts-${name}.png)`);
      if (renderer === 'webgl') {
        const note = await rendererNote();
        const says = /can.t use fonts you installed/.test(note);
        if (says !== !drawable) await fail(`${font}: the Renderer setting ${says ? 'says' : 'does not say'} that the font falls back to DOM`);
        pass(`${font}: the Renderer setting ${says ? 'explains the DOM fallback' : 'has no fallback note'}`);
      }
    }
  }
  console.log('Now look at the fonts-*.png screenshots: the font\'s own letters, joined box lines, and the Nerd Font\'s icons as pictures (the system font shows boxes for them).');
}
