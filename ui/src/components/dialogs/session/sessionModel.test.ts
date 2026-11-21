import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { isValidHost } from './sessionModel';

// The same cases as the native isValidHostName (native/tests/app/cli_tests.cpp): the two checks must agree.
const cases = JSON.parse(
  readFileSync(fileURLToPath(new URL('../../../../../native/tests/fixtures/host-names.json', import.meta.url)), 'utf8')
) as { valid: string[]; invalid: string[] };

describe('isValidHost (shared with the native check)', () => {
  it('has cases', () => {
    expect(cases.valid.length).toBeGreaterThanOrEqual(10);
    expect(cases.invalid.length).toBeGreaterThanOrEqual(10);
  });
  it.each(cases.valid)('accepts %j', (h) => expect(isValidHost(h)).toBe(true));
  it.each(cases.invalid)('refuses %j', (h) => expect(isValidHost(h)).toBe(false));
});
