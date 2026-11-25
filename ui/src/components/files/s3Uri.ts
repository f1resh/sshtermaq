/**
 * The full s3:// address of a path in an S3 file manager, as the AWS CLI and most S3 tools take it
 * (`aws s3 ls s3://bucket/folder/`). Without a bucket in the session the pane's paths are `/bucket/key`; with one
 * they are `/key` inside that bucket (S3Fs::locate). Folders end with "/".
 */
export function s3Uri(path: string, isDir: boolean, sessionBucket: string): string | null {
  const bucket = sessionBucket.replace(/^\/+|\/+$/g, '');
  const rest = path.replace(/^\/+|\/+$/g, '');
  let b: string;
  let key: string;
  if (bucket) {
    b = bucket;
    key = rest;
  } else {
    if (!rest) return null;  // the list of buckets
    const slash = rest.indexOf('/');
    b = slash < 0 ? rest : rest.slice(0, slash);
    key = slash < 0 ? '' : rest.slice(slash + 1);
  }
  if (!key) return `s3://${b}/`;
  return `s3://${b}/${key}${isDir ? '/' : ''}`;
}
