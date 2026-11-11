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
  app: async ({ page, os }, use) => {
    await page.addInitScript(({ platform, userAgent }) => {
      Object.defineProperty(Navigator.prototype, 'platform', { get: () => platform });
      Object.defineProperty(Navigator.prototype, 'userAgent', { get: () => userAgent });
    }, navigatorOf[os]);
    const errors: string[] = [];
    page.on('pageerror', (e) => errors.push(e.message));
    await page.goto('/');
    await expect(page.getByRole('tree', { name: 'Sessions' })).toBeVisible();
    await use(page);
    expect(errors, 'uncaught errors in the page').toEqual([]);
  }
});

export { expect };

/** The platform's main modifier: Ctrl on Windows, ⌘ on macOS. */
export const mod = (os: HostOs) => (os === 'darwin' ? 'Meta' : 'Control');
