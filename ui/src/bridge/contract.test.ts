/**
 * Bridge contract: API_SCHEMA (src/shared/ipc.ts) is the one list of channels. These tests check that both backends
 * answer exactly those channels, with the right kind:
 * - the native host: `bridge.handle*` / `bridge.on` registrations and `emit` calls in native/src, read as text;
 * - the mock backend (src/bridge/mock), which the browser previews and the Playwright tests (ui/e2e) run against.
 */
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { API_SCHEMA, channelOf } from '@shared/ipc';
import type { MockContext } from './mock';
import { registerConnMocks } from './mock/conn';
import { registerCoreMocks } from './mock/core';
import { registerImporterMocks } from './mock/importer';
import { registerKeysMocks } from './mock/keys';
import { registerLocalMocks } from './mock/local';
import { registerSftpMocks } from './mock/sftp';

type Kind = 'invoke' | 'send' | 'event' | 'local';

const schema = new Map<string, Kind>();
for (const [ns, methods] of Object.entries(API_SCHEMA))
  for (const [method, kind] of Object.entries(methods as Record<string, Kind>)) schema.set(channelOf(ns, method), kind);

const channelsOf = (...kinds: Kind[]) => [...schema].filter(([, k]) => kinds.includes(k)).map(([ch]) => ch).sort();

/** Every file below `dir` whose name ends with one of `exts`. */
function sourceFiles(dir: string, exts: string[]): string[] {
  return readdirSync(dir, { withFileTypes: true, recursive: true })
    .filter((e) => e.isFile() && exts.some((x) => e.name.endsWith(x)))
    .map((e) => join(e.parentPath, e.name));
}

/** All matches of `re`'s first group in the given files. */
function collect(files: string[], re: RegExp): Map<string, string> {
  const found = new Map<string, string>();
  for (const f of files) for (const m of readFileSync(f, 'utf8').matchAll(re)) found.set(m[2] ?? m[1], m[1]);
  return found;
}

const sorted = (it: Iterable<string>) => [...it].sort();

describe('native host', () => {
  const files = sourceFiles(fileURLToPath(new URL('../../../native/src', import.meta.url)), ['.cpp', '.mm', '.h']);
  // bridge.handle("ns:method", ...), handleSync, handleAsync: invoke; bridge.on("ns:method", ...): send.
  const handlers = collect(files, /\.(handle|handleSync|handleAsync|on)\(\s*"(\w+:\w+)"/g);
  const emitted = new Set(collect(files, /emit\(\s*"(\w+:\w+)"/g).keys());

  it('handles every invoke and send channel, and nothing else', () => {
    expect(sorted(handlers.keys())).toEqual(channelsOf('invoke', 'send'));
  });

  it('registers each channel with its kind (on() for send, handle*() for invoke)', () => {
    const wrong = [...handlers].filter(([ch, how]) => (schema.get(ch) === 'send') !== (how === 'on'));
    expect(wrong).toEqual([]);
  });

  it('emits only events of the schema, and every one of them', () => {
    expect(sorted(emitted)).toEqual(channelsOf('event'));
  });
});

describe('mock backend', () => {
  const invokes = new Set<string>();
  const sends = new Set<string>();
  const ctx: MockContext = { handle: (ch) => void invokes.add(ch), on: (ch) => void sends.add(ch), emit: () => undefined };
  for (const register of [registerCoreMocks, registerConnMocks, registerSftpMocks, registerLocalMocks, registerImporterMocks, registerKeysMocks])
    register(ctx);
  const dir = fileURLToPath(new URL('./mock', import.meta.url));
  const mockFiles = sourceFiles(dir, ['.ts']).filter((f) => !f.endsWith('.test.ts'));
  const emitted = new Set(collect(mockFiles, /emit\(\s*'(\w+:\w+)'/g).keys());

  it('answers every invoke channel', () => {
    expect(channelsOf('invoke').filter((ch) => !invokes.has(ch))).toEqual([]);
  });

  it('answers every send channel', () => {
    expect(channelsOf('send').filter((ch) => !sends.has(ch))).toEqual([]);
  });

  it('registers only channels of the schema, with their kind', () => {
    expect(sorted(invokes).filter((ch) => schema.get(ch) !== 'invoke')).toEqual([]);
    expect(sorted(sends).filter((ch) => schema.get(ch) !== 'send')).toEqual([]);
  });

  it('emits only events of the schema', () => {
    expect(sorted(emitted).filter((ch) => schema.get(ch) !== 'event')).toEqual([]);
  });
});
