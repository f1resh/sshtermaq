import { describe, expect, it } from 'vitest';
import { createDefaultSession, normalizeSession } from '@shared/defaults';

describe('"Allow old SHA-1 RSA" (ssh.allowSha1Rsa)', () => {
  it('is off for new sessions and for sessions saved before it existed', () => {
    expect(createDefaultSession().ssh.allowSha1Rsa).toBe(false);
    const old = { name: 'saved by 1.1.0', host: 'h.example', ssh: { kex: [], hostKeyAlgorithms: ['ssh-ed25519'] } };
    const s = normalizeSession(old);
    expect(s.ssh.allowSha1Rsa).toBe(false);
    expect(s.ssh.hostKeyAlgorithms).toEqual(['ssh-ed25519']);
  });

  it('keeps its value through a save and a load, and from the Default Settings template', () => {
    const on = createDefaultSession({ host: 'old.example' });
    on.ssh.allowSha1Rsa = true;
    expect(normalizeSession(JSON.parse(JSON.stringify(on))).ssh.allowSha1Rsa).toBe(true);
    const template = createDefaultSession();
    template.ssh.allowSha1Rsa = true;
    expect(normalizeSession({ host: 'new.example' }, template).ssh.allowSha1Rsa).toBe(true);
  });
});
