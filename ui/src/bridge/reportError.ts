/**
 * The UI's own errors in the app log (logs/sshtermaq.log, through app:log): uncaught errors, unhandled promise
 * rejections and render errors (components/ErrorBoundary.tsx). Without this they only reach the web view's console,
 * which users never see. Repeats are dropped and the number of reports is capped, so an error in a loop cannot fill
 * the log.
 */
import { api } from './api';

const MAX_REPORTS = 50;
const MAX_LENGTH = 4000;
const reported = new Set<string>();

/** Message and stack of an error, whatever was thrown. Chromium's stack starts with the message, WebKit's does not. */
export function describeError(error: unknown): string {
  if (error instanceof Error) {
    const head = `${error.name}: ${error.message}`;
    const stack = error.stack ?? '';
    return stack.startsWith(head) ? stack : stack ? `${head}\n${stack}` : head;
  }
  if (typeof error === 'string') return error;
  try {
    return JSON.stringify(error) ?? String(error);
  } catch {
    return String(error);
  }
}

/** Writes `error` to the app log, once per distinct text, at most MAX_REPORTS times per page load. */
export function reportError(where: string, error: unknown, level: 'error' | 'warn' = 'error'): void {
  const text = `${where}: ${describeError(error)}`.slice(0, MAX_LENGTH);
  if (reported.has(text) || reported.size >= MAX_REPORTS) return;
  reported.add(text);
  try {
    api.app.log(level, text);
  } catch {
    // The bridge itself is broken: there is nowhere else to write to.
  }
}

/** Reports errors nothing else caught. Call once, before the UI renders. */
export function installErrorReporting(target: Pick<Window, 'addEventListener'> = window): void {
  target.addEventListener('error', (ev: ErrorEvent) => reportError('Uncaught error', ev.error ?? ev.message));
  target.addEventListener('unhandledrejection', (ev: PromiseRejectionEvent) => reportError('Unhandled promise rejection', ev.reason));
}

/** Test helper: forget what was reported. */
export function resetReportedErrors(): void {
  reported.clear();
}
