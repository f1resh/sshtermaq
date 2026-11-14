import { afterEach, describe, expect, it, vi } from 'vitest';

/** The defaults module as it loads on macOS or Windows (`isMac` is read when the module loads). */
async function defaultsOn(mac: boolean) {
  vi.resetModules();
  vi.doMock('./platform', async (original) => ({ ...(await original<typeof import('./platform')>()), isMac: mac }));
  return import('./defaults');
}

afterEach(() => {
  vi.doUnmock('./platform');
  vi.resetModules();
});

describe('"Leave out .DS_Store and ._ files"', () => {
  it('is on by default on macOS', async () => {
    const d = await defaultsOn(true);
    expect(d.createDefaultSettings().files.skipMacFiles).toBe(true);
    expect(d.normalizeSettings({}).files.skipMacFiles).toBe(true);
  });

  it('is off by default on Windows', async () => {
    const d = await defaultsOn(false);
    expect(d.createDefaultSettings().files.skipMacFiles).toBe(false);
    expect(d.normalizeSettings({}).files.skipMacFiles).toBe(false);
  });

  it('keeps what existing users have saved, on either system', async () => {
    const win = await defaultsOn(false);
    expect(win.normalizeSettings({ files: { skipMacFiles: true } }).files.skipMacFiles).toBe(true);
    const mac = await defaultsOn(true);
    expect(mac.normalizeSettings({ files: { skipMacFiles: false } }).files.skipMacFiles).toBe(false);
  });
});
