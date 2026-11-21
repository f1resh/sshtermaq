import { afterEach, describe, expect, it, vi } from 'vitest';
import { canvasDrawsFont, resetCanvasFontCache, widthsMatch, type FontMeasurer } from './canvasFont';

/** A measurer with fixed widths per font list (as the CSS shorthand canvasDrawsFont builds). */
const fake = (canvas: number | null, dom: number | null) => {
  const m: FontMeasurer = { canvas: vi.fn(() => canvas), dom: vi.fn(() => dom) };
  return m;
};

describe('canvasFont', () => {
  afterEach(() => {
    resetCanvasFontCache();
    vi.restoreAllMocks();
  });

  it('widths within half a pixel match', () => {
    expect(widthsMatch(1132.8, 1132.8)).toBe(true);
    expect(widthsMatch(1132.8, 1133.2)).toBe(true);
    expect(widthsMatch(1386.2, 1125.1)).toBe(false);  // a canvas fallback of another width (Times for Cascadia Mono NF)
  });

  it('a font the canvas and the page both draw is drawable', () => {
    expect(canvasDrawsFont('Menlo', fake(963.2, 963.2))).toBe(true);
  });

  it('a font only the page finds is not (a user-installed font in WKWebView)', () => {
    vi.spyOn(console, 'info').mockImplementation(() => undefined);
    expect(canvasDrawsFont("'Cascadia Mono NF', monospace", fake(1155.5, 937.6))).toBe(false);
  });

  it('measures the font list at a fixed size and remembers the answer per list', () => {
    const m = fake(500, 500);
    canvasDrawsFont("'Fira Code', monospace", m);
    canvasDrawsFont("'Fira Code', monospace", m);
    expect(m.canvas).toHaveBeenCalledTimes(1);
    expect(m.canvas).toHaveBeenCalledWith("64px 'Fira Code', monospace", expect.any(String));
    canvasDrawsFont('Menlo', m);
    expect(m.canvas).toHaveBeenCalledTimes(2);
  });

  it('without a canvas or a page it leaves the renderer alone and asks again later', () => {
    const none = fake(null, 500);
    expect(canvasDrawsFont('Menlo', none)).toBe(true);
    expect(canvasDrawsFont('Menlo', fake(500, null))).toBe(true);
    const later = fake(500, 300);
    vi.spyOn(console, 'info').mockImplementation(() => undefined);
    expect(canvasDrawsFont('Menlo', later)).toBe(false);
  });
});
