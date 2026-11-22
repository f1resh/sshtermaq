import { describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';
import { linkSeconds, MAX_LINK_SECONDS } from '@/components/files/LinkDialog';
import { fallbackSessionName, isValidBucketName, s3EndpointError, sectionOfField, switchProtocol, validateSession, visibleSections } from './sessionModel';
import { applyS3Preset } from './sections/S3Sections';

function s3(patch: Partial<SessionConfig['s3']> = {}): SessionConfig {
  const s = switchProtocol(createDefaultSession(), 's3');
  return { ...s, s3: { ...s.s3, ...patch } };
}

describe('S3 sessions in the editor', () => {
  it('open as files and show only the S3 sections', () => {
    const s = s3();
    expect(s.protocol).toBe('s3');
    expect(s.openAs).toBe('files');
    const ids = visibleSections('s3').map((x) => x.id);
    expect(ids).toEqual(['session', 's3']);
    expect(visibleSections('ssh').map((x) => x.id)).toContain('terminal');
  });

  it('validates the endpoint, bucket, region, parts and proxy', () => {
    expect(validateSession(s3(), { sessions: [] }).errors).toEqual({});
    expect(s3EndpointError('')).toBeNull();
    expect(s3EndpointError('https://s3.example.com')).toBeNull();
    expect(s3EndpointError('http://localhost:9000/')).toBeNull();
    expect(s3EndpointError('https://[::1]:9000')).toBeNull();
    expect(s3EndpointError('ftp://x')).not.toBeNull();
    expect(s3EndpointError('https://host/path')).not.toBeNull();
    expect(s3EndpointError('https://bad host')).not.toBeNull();
    expect(s3EndpointError('https://h:99999')).toBe('Port must be 1–65535');
    const v = validateSession(s3({ endpoint: 'http://minio:9000', bucket: 'Bad Name', partSizeMiB: 4, parallelParts: 0, region: 'eu west' }), {
      sessions: []
    });
    expect(Object.keys(v.errors).sort()).toEqual(['s3.bucket', 's3.parallelParts', 's3.partSizeMiB', 's3.region']);
    expect(v.warnings['s3.endpoint']).toMatch(/unencrypted/);
    expect(validateSession(s3({ auth: { method: 'anonymous', accessKeyId: '', loginId: '', profile: '' } }), { sessions: [] }).errors['s3.bucket']).toMatch(
      /Anonymous/
    );
    const withProxy = s3({ systemProxy: false });
    withProxy.proxy = { ...withProxy.proxy, type: 'socks5', host: 'p', port: 1080 };
    expect(validateSession(withProxy, { sessions: [] }).errors['proxy.type']).toBeDefined();
    expect(sectionOfField('s3.bucket', 's3')).toBe('session');
    expect(sectionOfField('s3.partSizeMiB', 's3')).toBe('s3');
    expect(sectionOfField('proxy.host', 's3')).toBe('s3');
  });

  it('bucket names', () => {
    for (const ok of ['abc', 'my-bucket.logs', 'Legacy_Bucket', 'a1b']) expect(isValidBucketName(ok)).toBe(true);
    for (const bad of ['ab', '-abc', 'abc-', 'a b', 'x&calc.exe&', '']) expect(isValidBucketName(bad)).toBe(false);
  });

  it('names an unnamed session after its bucket or endpoint', () => {
    expect(fallbackSessionName(s3({ bucket: 'photos' }))).toBe('s3://photos');
    expect(fallbackSessionName(s3({ endpoint: 'https://minio.lan:9000/' }))).toBe('minio.lan:9000');
    expect(fallbackSessionName(s3())).toBe('S3');
  });

  it('service presets fill the endpoint and region only while untouched', () => {
    const r2 = applyS3Preset(s3().s3, 'r2');
    expect(r2.endpoint).toBe('https://<account-id>.r2.cloudflarestorage.com');
    expect(r2.region).toBe('auto');
    const minio = applyS3Preset(r2, 'minio');
    expect(minio.endpoint).toBe('http://localhost:9000');
    const own = applyS3Preset({ ...minio, endpoint: 'http://nas:9000', region: 'home' }, 'wasabi');
    expect(own.endpoint).toBe('http://nas:9000');
    expect(own.region).toBe('home');
    expect(own.preset).toBe('wasabi');
  });
});

describe('Copy link expiry', () => {
  it('accepts 1 minute to 7 days', () => {
    expect(linkSeconds('12', 'hours')).toBe(12 * 3600);
    expect(linkSeconds('7', 'days')).toBe(MAX_LINK_SECONDS);
    expect(linkSeconds('8', 'days')).toBeNull();
    expect(linkSeconds('0', 'minutes')).toBeNull();
    expect(linkSeconds('1.5', 'hours')).toBeNull();
    expect(linkSeconds('', 'hours')).toBeNull();
    expect(linkSeconds(' 30 ', 'minutes')).toBe(1800);
  });
});
