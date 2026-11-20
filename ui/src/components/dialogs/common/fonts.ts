/**
 * Terminal font helpers: curated monospace suggestions with where to get them, the fonts installed on this computer
 * (from the host: app:listFonts, with the monospace ones marked), what the font picker offers, the note for a chosen
 * font that is not installed, and CSS font-family string helpers. The picker logic is pure (fonts.test.ts).
 */
import { api } from '@/bridge/api';
import { isMac } from '@shared/platform';
import type { InstalledFont } from '@shared/types';

/** Monospace families that come with the OS. */
const OS_MONO_FONTS = isMac ? ['Menlo', 'Monaco', 'Andale Mono', 'Courier New'] : ['Cascadia Mono', 'Cascadia Code', 'Consolas', 'Lucida Console', 'Courier New'];

/** Popular monospace families (shown first; the OS's own ones at the top). */
export const MONO_FONT_SUGGESTIONS = [
  ...OS_MONO_FONTS,
  'JetBrains Mono',
  'Fira Code',
  'Fira Mono',
  'Source Code Pro',
  'Hack',
  'Iosevka',
  'IBM Plex Mono',
  'Ubuntu Mono',
  'Roboto Mono',
  'DejaVu Sans Mono',
  'Inconsolata',
  'Victor Mono',
  'MesloLGS NF',
  'CaskaydiaCove Nerd Font'
] as const;

/** Where to get the suggested fonts (all free). */
export const FONT_DOWNLOADS: Readonly<Record<string, string>> = {
  'Cascadia Mono': 'https://github.com/microsoft/cascadia-code/releases',
  'Cascadia Code': 'https://github.com/microsoft/cascadia-code/releases',
  'JetBrains Mono': 'https://www.jetbrains.com/lp/mono/',
  'Fira Code': 'https://github.com/tonsky/FiraCode',
  'Fira Mono': 'https://fonts.google.com/specimen/Fira+Mono',
  'Source Code Pro': 'https://fonts.google.com/specimen/Source+Code+Pro',
  Hack: 'https://sourcefoundry.org/hack/',
  Iosevka: 'https://typeof.net/Iosevka/',
  'IBM Plex Mono': 'https://fonts.google.com/specimen/IBM+Plex+Mono',
  'Ubuntu Mono': 'https://fonts.google.com/specimen/Ubuntu+Mono',
  'Roboto Mono': 'https://fonts.google.com/specimen/Roboto+Mono',
  'DejaVu Sans Mono': 'https://dejavu-fonts.github.io/',
  Inconsolata: 'https://fonts.google.com/specimen/Inconsolata',
  'Victor Mono': 'https://rubjo.github.io/victor-mono/',
  'MesloLGS NF': 'https://github.com/romkatv/powerlevel10k#meslo-nerd-font-patched-for-powerlevel10k',
  'CaskaydiaCove Nerd Font': 'https://www.nerdfonts.com/font-downloads'
};

let installedCache: Promise<InstalledFont[]> | null = null;

/** The fonts installed on this computer (asked once; [] when the host cannot list them). */
export function queryInstalledFonts(): Promise<InstalledFont[]> {
  installedCache ??= api.app
    .listFonts()
    .then((list) => (Array.isArray(list) ? list : []))
    .catch(() => {
      installedCache = null;
      return [];
    });
  return installedCache;
}

const same = (a: string, b: string) => a.localeCompare(b, undefined, { sensitivity: 'accent' }) === 0;

/** The installed family of that name (case does not matter), if any. */
export function findInstalled(family: string, installed: readonly InstalledFont[]): InstalledFont | undefined {
  return installed.find((f) => same(f.family, family));
}

export interface FontOption {
  value: string;
  group: string;
  /** A suggestion that is not installed: shown, not pickable. */
  disabled?: boolean;
  /** Where to get it. */
  url?: string;
}

export const FONT_GROUPS = { mono: 'Monospace fonts', missing: 'Not installed (free downloads)', other: 'Other installed fonts' } as const;

/**
 * What the font picker lists: the installed monospace fonts, then the suggestions that are not installed (greyed,
 * with a download link), then the other installed fonts (proportional ones look wrong in a terminal, but stay
 * pickable). Before the list has loaded (null): the suggestions alone.
 */
export function fontOptions(installed: readonly InstalledFont[] | null): FontOption[] {
  if (installed === null) return MONO_FONT_SUGGESTIONS.map((f) => ({ value: f, group: FONT_GROUPS.mono }));
  const mono = installed.filter((f) => f.monospace).map((f) => ({ value: f.family, group: FONT_GROUPS.mono }));
  const missing = MONO_FONT_SUGGESTIONS.filter((f) => !findInstalled(f, installed)).map((f) => ({
    value: f,
    group: FONT_GROUPS.missing,
    disabled: true,
    url: FONT_DOWNLOADS[f]
  }));
  const other = installed.filter((f) => !f.monospace).map((f) => ({ value: f.family, group: FONT_GROUPS.other }));
  return [...mono, ...missing, ...other];
}

/**
 * The chosen font is not installed: its name and where to get it, for the note under the picker. Null when it is
 * installed, when it is a generic family (monospace, ...), or while the list is unknown (null or empty: the host
 * could not list fonts, so nothing can be said).
 */
export function missingFont(css: string, installed: readonly InstalledFont[] | null): { family: string; url?: string } | null {
  const family = primaryFamily(css);
  if (!family || !installed?.length || GENERIC.has(family.toLowerCase()) || findInstalled(family, installed)) return null;
  const known = Object.keys(FONT_DOWNLOADS).find((k) => same(k, family));
  return { family, url: known ? FONT_DOWNLOADS[known] : undefined };
}

/** First family of a CSS font-family list, unquoted ("'Cascadia Mono', Consolas" -> "Cascadia Mono"). */
export function primaryFamily(css: string): string {
  const first = splitFamilies(css)[0] ?? '';
  return first;
}

/** Splits a CSS font-family list into unquoted family names. */
export function splitFamilies(css: string): string[] {
  const out: string[] = [];
  let cur = '';
  let quote: string | null = null;
  for (const ch of css) {
    if (quote) {
      if (ch === quote) quote = null;
      else cur += ch;
    } else if (ch === '"' || ch === "'") {
      quote = ch;
    } else if (ch === ',') {
      if (cur.trim()) out.push(cur.trim());
      cur = '';
    } else {
      cur += ch;
    }
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

const GENERIC = new Set(['monospace', 'serif', 'sans-serif', 'cursive', 'fantasy', 'system-ui', 'ui-monospace']);

/** CSS font-family value for a picked family, with a monospace fallback. */
export function familyToCss(family: string): string {
  const f = family.trim();
  if (!f) return '';
  if (f.includes(',')) return f; // already a list
  if (GENERIC.has(f.toLowerCase())) return f;
  return `'${f.replace(/'/g, "\\'")}', monospace`;
}
