import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { fileURLToPath } from 'node:url';
import { licenses } from './scripts/licenses';

const r = (p: string) => fileURLToPath(new URL(p, import.meta.url));

export default defineConfig({
  plugins: [react(), licenses()],
  // Assets are served by the native host: https://app.sshtermaq.local/ (Windows, WebView2), sshtermaq://app/ (macOS).
  base: './',
  resolve: {
    alias: {
      '@': r('./src'),
      '@shared': r('./src/shared')
    }
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // WebView2 (Chromium) on Windows, the system WebKit (Safari 16+) on macOS.
    target: ['chrome120', 'safari16'],
    sourcemap: true,
    chunkSizeWarningLimit: 2000
  },
  server: {
    port: 5173,
    strictPort: true
  },
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts', 'tests/**/*.test.ts'],
    // `npm run coverage` (scripts/coverage.mjs): the UI's own code, without the tests and the mock backend.
    coverage: {
      provider: 'v8',
      include: ['src/**/*.{ts,tsx}'],
      exclude: ['src/**/*.test.ts', 'src/bridge/mock/**', 'src/**/*.d.ts'],
      reporter: ['json'],
      reportsDirectory: 'coverage/unit'
    }
  }
});
