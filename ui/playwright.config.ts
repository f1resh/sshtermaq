/**
 * Playwright tests of the UI against the mock backend (src/bridge/mock), in a real browser: `npm run test:e2e`.
 * Vite serves the UI on its own port; every test starts with the mock's seed data (e2e/fixtures.ts clears the
 * page's storage).
 * Each test runs twice, once as on Windows and once as on macOS (the UI picks its layout from the browser, see
 * e2e/fixtures.ts). See docs/TESTING.md.
 */
import { defineConfig } from '@playwright/test';
import type { HostOs } from './e2e/fixtures';

const port = 5174;   // not the dev server's 5173, so a running `npm run dev` does not get in the way

// The system's Edge on Windows (as scripts/shot.mjs), Chrome elsewhere; PW_CHANNEL overrides it.
const channel = process.env.PW_CHANNEL ?? (process.platform === 'win32' ? 'msedge' : 'chrome');

export default defineConfig<{ os: HostOs }>({
  testDir: 'e2e',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: [['list'], ['html', { open: 'never' }]],
  use: {
    baseURL: `http://localhost:${port}/`,
    channel,
    viewport: { width: 1400, height: 900 },
    colorScheme: 'dark',
    // A trace of each failing test is saved by e2e/fixtures.ts (its own browser context: see there).
  },
  projects: [
    { name: 'windows', use: { os: 'win32' } },
    { name: 'macos', use: { os: 'darwin' } }
  ],
  webServer: {
    command: `npx vite --port ${port} --strictPort`,
    url: `http://localhost:${port}/`,
    reuseExistingServer: !process.env.CI,
    timeout: 60_000
  }
});
