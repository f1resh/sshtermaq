import { beforeEach, describe, expect, it, vi } from 'vitest';
import { finishUnlock, isLockedError, requestUnlock, useVault, withUnlock } from './vault';

const LOCKED = new Error('Credentials are locked: unlock them with the master password first');

/** Answers the unlock dialog as soon as it opens. */
function answerUnlock(unlocked: boolean) {
  const stop = useVault.subscribe((s) => {
    if (s.unlockRequest) {
      stop();
      queueMicrotask(() => finishUnlock(unlocked));
    }
  });
}

describe('Credentials: unlock and retry', () => {
  beforeEach(() => useVault.setState({ unlockRequest: null }));

  it('recognises the host\'s "locked" answer', () => {
    expect(isLockedError(LOCKED)).toBe(true);
    expect(isLockedError('Credentials are locked: …')).toBe(true);
    expect(isLockedError(new Error('A key named "x" already exists'))).toBe(false);
  });

  it('asks to unlock once, then runs the call again', async () => {
    const fn = vi.fn().mockRejectedValueOnce(LOCKED).mockResolvedValueOnce('added');
    answerUnlock(true);
    await expect(withUnlock(fn)).resolves.toBe('added');
    expect(fn).toHaveBeenCalledTimes(2);
    expect(useVault.getState().unlockRequest).toBeNull();
  });

  it('cancelled: the "locked" error stands; other errors never ask', async () => {
    const locked = vi.fn().mockRejectedValue(LOCKED);
    answerUnlock(false);
    await expect(withUnlock(locked)).rejects.toThrow('Credentials are locked');
    expect(locked).toHaveBeenCalledTimes(1);

    const other = vi.fn().mockRejectedValue(new Error('Name taken'));
    await expect(withUnlock(other)).rejects.toThrow('Name taken');
    expect(useVault.getState().unlockRequest).toBeNull();
  });

  it('two requests at once share the dialog and both get its answer', async () => {
    const a = requestUnlock();
    const b = requestUnlock();
    finishUnlock(true);
    await expect(Promise.all([a, b])).resolves.toEqual([true, true]);
  });
});
