/**
 * Font family combo box: the installed monospace fonts, the suggested ones that are not installed (greyed, with a
 * download link), then the other installed fonts. The list comes from the host (app:listFonts) and is loaded when the
 * picker appears, so a chosen font that is not installed gets a note under the field. Options render in their own
 * typeface.
 */
import { useEffect, useMemo, useState } from 'react';
import { api } from '@/bridge/api';
import type { InstalledFont } from '@shared/types';
import { ComboInput, type ComboOption } from './controls';
import { familyToCss, fontOptions, missingFont, primaryFamily, queryInstalledFonts, type FontOption } from './fonts';

const openDownload = (url: string) => void api.app.openExternal(url).catch(() => undefined);

export function FontPicker({
  value,
  onChange,
  placeholder,
  id
}: {
  /** CSS font-family list. */
  value: string;
  onChange: (cssFontFamily: string) => void;
  placeholder?: string;
  id?: string;
}) {
  const [installed, setInstalled] = useState<InstalledFont[] | null>(null);
  const [text, setText] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    void queryInstalledFonts().then((list) => alive && setInstalled(list));
    return () => {
      alive = false;
    };
  }, []);

  const options = useMemo<FontOption[]>(() => fontOptions(installed), [installed]);

  // Drop the typed text when the value is changed from outside (reset, undo, ...).
  useEffect(() => {
    setText((t) => (t !== null && familyToCss(t) !== value ? null : t));
  }, [value]);

  // Show the friendly primary family while the user is not typing.
  const display = text ?? primaryFamily(value);
  const missing = missingFont(value, installed);

  return (
    <div className="font-picker">
      <ComboInput
        id={id}
        value={display}
        placeholder={placeholder}
        options={options}
        filter={text !== null}
        onChange={(v) => {
          setText(v);
          onChange(familyToCss(v));
        }}
        renderOption={(o: ComboOption) => {
          const url = (o as FontOption).url;
          return (
            <span className="font-option">
              <span className="font-option__name" style={o.disabled ? undefined : { fontFamily: `'${o.value}', var(--font-ui)` }}>
                {o.value}
              </span>
              {o.disabled ? (
                <span className="font-option__missing">
                  not installed
                  {url && (
                    <button
                      type="button"
                      className="font-option__get"
                      onMouseDown={(e) => {
                        e.preventDefault();
                        e.stopPropagation();
                        openDownload(url);
                      }}
                    >
                      Get it
                    </button>
                  )}
                </span>
              ) : (
                <span className="font-option__sample" style={{ fontFamily: `'${o.value}', monospace` }}>
                  {'{ } 0O 1lI ->'}
                </span>
              )}
            </span>
          );
        }}
        emptyText="No matching fonts"
      />
      {missing && (
        <div className="font-picker__note" role="note">
          {missing.family} is not installed on this computer, so the terminal uses the default monospace font.
          {missing.url && (
            <>
              {' '}
              <button type="button" onClick={() => openDownload(missing.url!)}>
                Download {missing.family}
              </button>
            </>
          )}
        </div>
      )}
    </div>
  );
}
