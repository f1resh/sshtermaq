import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import '@xterm/xterm/css/xterm.css';
import './styles/global.css';
import { App } from './App';
import { installErrorReporting } from './bridge/reportError';
import { ErrorBoundary } from './components/ErrorBoundary';

installErrorReporting();

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </StrictMode>
);
