/**
 * The `test` every spec imports.
 *
 * Its `os` option (set per project in playwright.config.ts) makes the page look like Windows or macOS to
 * src/shared/platform.ts, whatever machine runs the tests: the UI then shows that system's layout (title bar, menus,
 * shortcuts) and words (Explorer / Finder, COM3 / /dev/cu.*, DPAPI / Keychain).
 *
 * Browser profile: Edge and Chrome on Windows give some profile folders (the disk cache, "Shared Dictionary")
 * permissions that only the browser's network sandbox can use, and Playwright then hangs for minutes trying to delete
 * the temporary profile of every test. So each worker uses one profile folder of its own in the system temp folder,
 * kept between runs and never deleted, and every test starts by clearing the site's stored data: the mock backend
 * keeps its state in localStorage, so each test starts from the mock's seed data.
 */
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test as base, chromium, expect, type BrowserContext, type Page } from '@playwright/test';

export type HostOs = 'win32' | 'darwin';

const navigatorOf: Record<HostOs, { platform: string; userAgent: string }> = {
  win32: {
    platform: 'Win32',
    userAgent:
      'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36 Edg/140.0.0.0'
  },
  darwin: {
    platform: 'MacIntel',
    userAgent:
      'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15'
  }
};

export const test = base.extend<{ os: HostOs; app: Page }>({
  os: ['win32', { option: true }],

  context: async ({ channel, viewport, colorScheme, baseURL, headless }, use, testInfo) => {
    const profile = join(tmpdir(), `sshtermaq-e2e-profile-${testInfo.parallelIndex}`);
    const context: BrowserContext = await chromium.launchPersistentContext(profile, { channel, viewport, colorScheme, baseURL, headless });
    const blank = context.pages()[0] ?? (await context.newPage());
    const cdp = await context.newCDPSession(blank);
    await cdp.send('Storage.clearDataForOrigin', { origin: new URL(baseURL!).origin, storageTypes: 'all' });
    await cdp.detach();
    await context.tracing.start({ screenshots: true, snapshots: true });
    await use(context);
    if (testInfo.status !== testInfo.expectedStatus) {
      const trace = testInfo.outputPath('trace.zip');
      await context.tracing.stop({ path: trace });
      testInfo.attachments.push({ name: 'trace', path: trace, contentType: 'application/zip' });
    } else {
      await context.tracing.stop();
    }
    await context.close();
  },

  page: async ({ context }, use) => {
    await use(context.pages()[0] ?? (await context.newPage()));
  },

  /** The UI, loaded and showing the welcome screen, as on `os`. Fails the test on uncaught errors in the page. */
  app: async ({ page, os }, use, testInfo) => {
    await page.addInitScript(({ platform, userAgent }) => {
      Object.defineProperty(Navigator.prototype, 'platform', { get: () => platform });
      Object.defineProperty(Navigator.prototype, 'userAgent', { get: () => userAgent });
    }, navigatorOf[os]);
    const errors: string[] = [];
    page.on('pageerror', (e) => errors.push(e.message));
    // `npm run coverage` (scripts/coverage.mjs) sets PW_COV: record which code of the UI each test runs.
    const covDir = process.env.PW_COV;
    if (covDir) await page.coverage.startJSCoverage({ resetOnNavigation: false });
    await page.goto('/');
    // The first load (Vite compiling on demand) can take longer than the usual 5 s while 3 workers run, more so
    // with coverage on: give it 15.
    await expect(page.getByRole('tree', { name: 'Sessions' })).toBeVisible({ timeout: 15_000 });
    await use(page);
    if (covDir) await saveCoverage(covDir, await page.coverage.stopJSCoverage(), testInfo.testId);
    expect(errors, 'uncaught errors in the page').toEqual([]);
  }
});

/**
 * Writes the V8 coverage of one test: the ranges per module in `<dir>/runs/<test>.json`, and each module's source once
 * (with Vite's inline source map) in `<dir>/sources/<hash>.js`, which scripts/coverage.mjs maps back to `src/`.
 */
async function saveCoverage(dir: string, entries: { url: string; source?: string; functions: unknown[] }[], testId: string): Promise<void> {
  mkdirSync(join(dir, 'runs'), { recursive: true });
  mkdirSync(join(dir, 'sources'), { recursive: true });
  const runs = [];
  for (const e of entries) {
    const path = new URL(e.url).pathname;
    if (!path.startsWith('/src/') || path.startsWith('/src/bridge/mock/') || !e.source) continue;
    const hash = createHash('sha1').update(e.source).digest('hex');
    const file = join(dir, 'sources', `${hash}.js`);
    if (!existsSync(file)) writeFileSync(file, e.source);
    runs.push({ path, hash, functions: e.functions });
  }
  writeFileSync(join(dir, 'runs', `${testId}.json`), JSON.stringify(runs));
}

export { expect };

/** The platform's main modifier: Ctrl on Windows, ⌘ on macOS. */
export const mod = (os: HostOs) => (os === 'darwin' ? 'Meta' : 'Control');
