#!/usr/bin/env node
/**
 * Renders the SSHTermaq logo (the drawing of src/components/welcome/Logo.tsx and native/res/tools/make_icon.py) into
 * the images the Microsoft Store needs, with headless Microsoft Edge:
 *
 *   node scripts/store-art.mjs --msix ../native/res/windows/msix   MSIX tiles (Square150x150Logo, Wide310x150Logo)
 *   node scripts/store-art.mjs --listing <folder>                  Store listing art (tile, box art, poster)
 *
 * Keep the SVG below in step with Logo.tsx when the logo changes.
 */
import { mkdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { chromium } from 'playwright-core';

const args = process.argv.slice(2);
const opt = (name) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : undefined;
};

const LOGO = `
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 120" width="100%" height="100%">
  <defs><filter id="g" x="-25%" y="-25%" width="150%" height="150%"><feGaussianBlur stdDeviation="5"/></filter></defs>
  <rect width="120" height="120" rx="26" fill="#08050c"/>
  <g fill="none" stroke-linecap="round" stroke-linejoin="round">
    <g filter="url(#g)" stroke-width="10" opacity="0.85">
      <g stroke="#e040fb"><rect x="22" y="24" width="72" height="68" rx="18"/><line x1="88" y1="86" x2="104" y2="102"/></g>
      <g stroke="#ff3db8"><path d="M38 46 L50 57 L38 68"/><line x1="57" y1="69" x2="74" y2="69"/></g>
    </g>
    <g stroke-width="6.5" opacity="0.85">
      <g stroke="#e040fb"><rect x="22" y="24" width="72" height="68" rx="18"/><line x1="88" y1="86" x2="104" y2="102"/></g>
      <g stroke="#ff3db8"><path d="M38 46 L50 57 L38 68"/><line x1="57" y1="69" x2="74" y2="69"/></g>
    </g>
    <g stroke-width="2.8">
      <g stroke="#ffe4ff"><rect x="22" y="24" width="72" height="68" rx="18"/><line x1="88" y1="86" x2="104" y2="102"/></g>
      <g stroke="#ffe0f2"><path d="M38 46 L50 57 L38 68"/><line x1="57" y1="69" x2="74" y2="69"/></g>
    </g>
  </g>
</svg>`;

/** The logo alone, `logo` px wide, centred in a transparent w x h image. */
const logoPage = (w, h, logo) => `
<html><body style="margin:0;width:${w}px;height:${h}px;display:grid;place-items:center;background:transparent">
  <div style="width:${logo}px;height:${logo}px">${LOGO}</div>
</body></html>`;

/** Listing art: the logo, the name and a line of text on the app's near-black background with a soft glow. */
const artPage = (w, h, { logo, title, tagline, column }) => `
<html><body style="margin:0;width:${w}px;height:${h}px;overflow:hidden;
  background:radial-gradient(ellipse at 50% ${column ? 38 : 50}%, #2a0f33 0%, #0d0712 55%, #08050c 100%);
  display:flex;flex-direction:${column ? 'column' : 'row'};align-items:center;justify-content:center;gap:${Math.round(logo * 0.18)}px;
  font-family:'Segoe UI Variable Display','Segoe UI',sans-serif;color:#fff">
  <div style="width:${logo}px;height:${logo}px;filter:drop-shadow(0 0 ${Math.round(logo * 0.12)}px rgba(224,64,251,.45))">${LOGO}</div>
  <div style="text-align:center">
    <div style="font-size:${title}px;font-weight:700;letter-spacing:-0.01em">SSHTermaq</div>
    ${tagline ? `<div style="font-size:${tagline}px;color:#ff3db8;margin-top:${Math.round(tagline * 0.3)}px">PuTTY-compatible SSH client</div>` : ''}
  </div>
</body></html>`;

const browser = await chromium.launch({ channel: 'msedge', headless: true });
try {
  const page = await browser.newPage();
  const render = async (file, w, h, html) => {
    await page.setViewportSize({ width: w, height: h });
    await page.setContent(html);
    await page.screenshot({ path: file, omitBackground: true, clip: { x: 0, y: 0, width: w, height: h } });
    console.log(file);
  };

  const msix = opt('msix');
  if (msix) {
    const dir = resolve(msix);
    mkdirSync(dir, { recursive: true });
    // The tiles keep a margin around the logo, as Windows' own do. The square app icons (Square44x44Logo,
    // StoreLogo) come from native/res/tools/make_icon.py --msix, which thickens the lines at small sizes.
    for (const [scale, f] of [[100, 1], [200, 2]]) {
      await render(join(dir, `Square150x150Logo.scale-${scale}.png`), 150 * f, 150 * f, logoPage(150 * f, 150 * f, 96 * f));
      await render(join(dir, `Wide310x150Logo.scale-${scale}.png`), 310 * f, 150 * f, logoPage(310 * f, 150 * f, 96 * f));
    }
  }

  const listing = opt('listing');
  if (listing) {
    const dir = resolve(listing);
    mkdirSync(dir, { recursive: true });
    await render(join(dir, 'tile-300.png'), 300, 300, logoPage(300, 300, 300));
    await render(join(dir, 'box-art-2160.png'), 2160, 2160, artPage(2160, 2160, { logo: 900, title: 230, tagline: 96, column: true }));
    await render(join(dir, 'poster-1440x2160.png'), 1440, 2160, artPage(1440, 2160, { logo: 760, title: 190, tagline: 80, column: true }));
    await render(join(dir, 'hero-1920x1080.png'), 1920, 1080, artPage(1920, 1080, { logo: 520, title: 170, tagline: 64, column: false }));
  }
  if (!msix && !listing) console.log('nothing to do: pass --msix <folder> and/or --listing <folder>');
} finally {
  // browser.close() can hang with Edge (see shot.mjs).
  await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
  process.exit(process.exitCode ?? 0);
}
