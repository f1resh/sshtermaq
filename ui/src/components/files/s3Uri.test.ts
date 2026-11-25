import { describe, expect, it } from 'vitest';
import { s3Uri } from './s3Uri';

describe('s3Uri: the full s3:// path of a file or folder', () => {
  it('without a bucket in the session the first part of the path is the bucket', () => {
    expect(s3Uri('/', true, '')).toBeNull();  // the list of buckets
    expect(s3Uri('/photos-2026', true, '')).toBe('s3://photos-2026/');
    expect(s3Uri('/photos-2026/index.html', false, '')).toBe('s3://photos-2026/index.html');
    expect(s3Uri('/photos-2026/january', true, '')).toBe('s3://photos-2026/january/');
    expect(s3Uri('/photos-2026/january/IMG_0001.jpg', false, '')).toBe('s3://photos-2026/january/IMG_0001.jpg');
  });

  it("with a bucket in the session the paths are inside it", () => {
    expect(s3Uri('/', true, 'media')).toBe('s3://media/');
    expect(s3Uri('/photos', true, 'media')).toBe('s3://media/photos/');
    expect(s3Uri('/photos/a.jpg', false, 'media')).toBe('s3://media/photos/a.jpg');
    expect(s3Uri('/a.jpg', false, '/media/')).toBe('s3://media/a.jpg');  // slashes around the session's bucket
  });

  it('keys keep their characters as they are (the AWS CLI takes them unencoded)', () => {
    expect(s3Uri('/b/my folder/a+b & c.txt', false, '')).toBe('s3://b/my folder/a+b & c.txt');
    expect(s3Uri('/b/отчёт.pdf', false, '')).toBe('s3://b/отчёт.pdf');
  });

  it('a trailing slash on a folder path is not doubled', () => {
    expect(s3Uri('/b/folder/', true, '')).toBe('s3://b/folder/');
    expect(s3Uri('/folder/', true, 'b')).toBe('s3://b/folder/');
  });
});
