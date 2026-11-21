/**
 * Whether a canvas can draw text in a terminal font. The WebGL renderer draws every glyph on a canvas first, and
 * WKWebView (macOS) gives a canvas only the system fonts and the page's own web fonts: a font the user installed
 * (a Nerd Font in ~/Library/Fonts, say) works for page text and the DOM renderer, but on a canvas it silently falls back
 * to another font, with boxes for its icons. termSession uses the DOM renderer for such a font.
 *
 * The check measures the same sample in the font list with a canvas and with a page element: equal widths mean both
 * found the same font. A list neither can find falls back the same way in both, so it counts as drawable. WebView2
 * (Windows) gives canvases the installed fonts, so there the answer is always yes.
 */

/** Wide and narrow letters, so a fallback of another width shows in the total. */
const SAMPLE = 'mmmmmiiiiiWWWWWlllll@@@@@00000';
const SIZE = 64;

export interface FontMeasurer {
  /** Width of `text` at `font` (CSS shorthand) on a canvas, or null without a canvas. */
  canvas(font: string, text: string): number | null;
  /** Width of `text` at `font` in a page element, or null without a page. */
  dom(font: string, text: string): number | null;
}

/** Widths of the same text agree when they are within half a pixel (subpixel layout rounds differently). */
export function widthsMatch(canvas: number, dom: number): boolean {
  return Math.abs(canvas - dom) <= 0.5;
}

const pageMeasurer: FontMeasurer = {
  canvas(font, text) {
    if (typeof document === 'undefined') return null;
    const ctx = document.createElement('canvas').getContext('2d');
    if (!ctx) return null;
    ctx.font = font;
    return ctx.measureText(text).width;
  },
  dom(font, text) {
    if (typeof document === 'undefined' || !document.body) return null;
    const span = document.createElement('span');
    span.textContent = text;
    span.style.cssText = 'position:absolute;left:-10000px;top:0;visibility:hidden;white-space:pre;letter-spacing:0;font-kerning:none';
    span.style.font = font;
    document.body.appendChild(span);
    const width = span.getBoundingClientRect().width;
    span.remove();
    return width;
  }
};

const cache = new Map<string, boolean>();

/** True when a canvas draws `fontFamily` (a CSS font-family list) as the page does; remembered per list. */
export function canvasDrawsFont(fontFamily: string, measure: FontMeasurer = pageMeasurer): boolean {
  const known = cache.get(fontFamily);
  if (known !== undefined) return known;
  const font = `${SIZE}px ${fontFamily}`;
  const canvas = measure.canvas(font, SAMPLE);
  const dom = measure.dom(font, SAMPLE);
  if (canvas === null || dom === null) return true;  // nothing to compare with: leave the renderer as set
  const ok = widthsMatch(canvas, dom);
  cache.set(fontFamily, ok);
  if (!ok) console.info(`[terminal] a canvas cannot draw ${fontFamily} (canvas ${canvas.toFixed(1)}px, page ${dom.toFixed(1)}px): using the DOM renderer`);
  return ok;
}

/** For tests: forget the remembered answers. */
export function resetCanvasFontCache(): void {
  cache.clear();
}
