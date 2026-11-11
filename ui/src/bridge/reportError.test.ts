import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { api } from './api';
import { describeError, installErrorReporting, reportError, resetReportedErrors } from './reportError';

describe('describeError', () => {
  it('keeps a stack that already starts with the message (Chromium)', () => {
    const e = new TypeError('x is undefined');
    e.stack = 'TypeError: x is undefined\n    at f (app.js:1:2)';
    expect(describeError(e)).toBe(e.stack);
  });

  it('puts the message in front of a stack without it (WebKit)', () => {
    const e = new Error('boom');
    e.stack = 'f@app.js:1:2';
    expect(describeError(e)).toBe('Error: boom\nf@app.js:1:2');
  });

  it('describes things that are not errors', () => {
    expect(describeError('plain text')).toBe('plain text');
    expect(describeError({ code: 3 })).toBe('{"code":3}');
    expect(describeError(undefined)).toBe('undefined');
  });
});

describe('reportError', () => {
  let log: ReturnType<typeof vi.spyOn>;
  beforeEach(() => {
    resetReportedErrors();
    log = vi.spyOn(api.app, 'log').mockImplementation(() => undefined);
  });
  afterEach(() => log.mockRestore());

  it('writes to the app log once per distinct error', () => {
    reportError('Uncaught error', 'a');
    reportError('Uncaught error', 'a');
    reportError('Uncaught error', 'b', 'warn');
    expect(log.mock.calls).toEqual([
      ['error', 'Uncaught error: a'],
      ['warn', 'Uncaught error: b']
    ]);
  });

  it('stops after 50 reports', () => {
    for (let i = 0; i < 60; i++) reportError('Loop', i);
    expect(log).toHaveBeenCalledTimes(50);
  });

  it('reports uncaught errors and unhandled rejections from the window', () => {
    const listeners = new Map<string, (ev: unknown) => void>();
    installErrorReporting({ addEventListener: ((type: string, cb: (ev: unknown) => void) => listeners.set(type, cb)) as Window['addEventListener'] });
    listeners.get('error')!({ error: 'broken', message: 'ignored' });
    listeners.get('unhandledrejection')!({ reason: 'rejected' });
    expect(log.mock.calls).toEqual([
      ['error', 'Uncaught error: broken'],
      ['error', 'Unhandled promise rejection: rejected']
    ]);
  });
});
