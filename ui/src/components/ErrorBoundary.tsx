/**
 * Last line of defence around the whole UI: a render error shows this page instead of an empty window, and goes to
 * the app log (bridge/reportError.ts) with the component stack.
 */
import { Component, type ErrorInfo, type ReactNode } from 'react';
import { describeError, reportError } from '@/bridge/reportError';
import { Button } from '@/components/ui';

interface State {
  error: unknown;
}

export class ErrorBoundary extends Component<{ children: ReactNode }, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: unknown): State {
    return { error: error ?? new Error('Unknown error') };
  }

  componentDidCatch(error: unknown, info: ErrorInfo): void {
    reportError('Render error', `${describeError(error)}\nComponents:${info.componentStack ?? ' (unknown)'}`);
  }

  render(): ReactNode {
    if (!this.state.error) return this.props.children;
    const message = this.state.error instanceof Error ? this.state.error.message : String(this.state.error);
    return (
      <div role="alert" style={{ height: '100vh', display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'var(--bg)', color: 'var(--text)', padding: 24 }}>
        <div style={{ maxWidth: 560, display: 'flex', flexDirection: 'column', gap: 12 }}>
          <h1 style={{ fontSize: 18, margin: 0 }}>Something went wrong</h1>
          <p style={{ margin: 0, color: 'var(--text-muted)' }}>
            The window ran into an error it could not recover from. The details are in the app log: sshtermaq.log in the
            logs folder of the data folder (Settings › Data &amp; backup).
          </p>
          <pre style={{ margin: 0, padding: 8, background: 'var(--bg-sunken)', color: 'var(--danger)', whiteSpace: 'pre-wrap', overflowWrap: 'anywhere', maxHeight: 160, overflow: 'auto' }}>{message}</pre>
          <div style={{ display: 'flex', gap: 8 }}>
            <Button variant="primary" onClick={() => window.location.reload()}>
              Reload the window
            </Button>
            <Button onClick={() => this.setState({ error: null })}>Try again</Button>
          </div>
        </div>
      </div>
    );
  }
}
