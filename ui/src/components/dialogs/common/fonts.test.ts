import { describe, expect, it } from 'vitest';
import type { InstalledFont } from '@shared/types';
import { FONT_DOWNLOADS, FONT_GROUPS, MONO_FONT_SUGGESTIONS, findInstalled, fontOptions, missingFont } from './fonts';

/** What a fresh Windows has (the owner's PC on 2026-11-14): none of the downloadable suggestions. */
const fresh: InstalledFont[] = [
  { family: 'Arial', monospace: false },
  { family: 'Cascadia Mono', monospace: true },
  { family: 'Consolas', monospace: true },
  { family: 'Segoe UI', monospace: false }
];

describe('the font picker', () => {
  it('lists installed monospace fonts, then the missing suggestions, then the other fonts', () => {
    const options = fontOptions(fresh);
    const groups = [...new Set(options.map((o) => o.group))];
    expect(groups).toEqual([FONT_GROUPS.mono, FONT_GROUPS.missing, FONT_GROUPS.other]);
    expect(options.filter((o) => o.group === FONT_GROUPS.mono).map((o) => o.value)).toEqual(['Cascadia Mono', 'Consolas']);
    expect(options.filter((o) => o.group === FONT_GROUPS.other).map((o) => o.value)).toEqual(['Arial', 'Segoe UI']);
  });

  it('marks a suggestion that is not installed: not pickable, with where to get it', () => {
    const jb = fontOptions(fresh).find((o) => o.value === 'JetBrains Mono');
    expect(jb).toMatchObject({ group: FONT_GROUPS.missing, disabled: true, url: FONT_DOWNLOADS['JetBrains Mono'] });
    // An installed suggestion is not in the "not installed" group.
    expect(fontOptions(fresh).some((o) => o.value === 'Consolas' && o.disabled)).toBe(false);
  });

  it('every suggestion has a download page or comes with an OS', () => {
    const osFonts = ['Menlo', 'Monaco', 'Andale Mono', 'Courier New', 'Consolas', 'Lucida Console'];
    for (const f of MONO_FONT_SUGGESTIONS) expect(FONT_DOWNLOADS[f] ?? (osFonts.includes(f) ? 'os' : undefined), f).toBeDefined();
  });

  it('before the list has loaded: the suggestions alone, all pickable', () => {
    const options = fontOptions(null);
    expect(options.map((o) => o.value)).toEqual([...MONO_FONT_SUGGESTIONS]);
    expect(options.some((o) => o.disabled)).toBe(false);
  });

  it('finds installed families whatever their letter case', () => {
    expect(findInstalled('consolas', fresh)?.family).toBe('Consolas');
    expect(findInstalled('JetBrains Mono', fresh)).toBeUndefined();
  });
});

describe('the note for a chosen font that is not installed', () => {
  it('names the font and its download page', () => {
    expect(missingFont("'JetBrains Mono', monospace", fresh)).toEqual({ family: 'JetBrains Mono', url: FONT_DOWNLOADS['JetBrains Mono'] });
    expect(missingFont("'My Own Font', monospace", fresh)).toEqual({ family: 'My Own Font', url: undefined });
  });

  it('says nothing for an installed font, a generic family, or an unknown list', () => {
    expect(missingFont("'Consolas', monospace", fresh)).toBeNull();
    expect(missingFont('monospace', fresh)).toBeNull();
    expect(missingFont("'JetBrains Mono', monospace", null)).toBeNull();
    expect(missingFont("'JetBrains Mono', monospace", [])).toBeNull();
  });
});
