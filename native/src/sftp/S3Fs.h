#pragma once
// S3 as a RemoteFs (the third, next to SftpFs and ScpFs), so the transfer engine, conflicts, find, synchronize and
// edit work unchanged on S3 sessions.
//
// Paths: without a bucket in the session, "/" lists the buckets and "/bucket/folder/file" is key "folder/file" of
// "bucket"; with one, "/" is the top of that bucket. "/" in key names acts as the folder separator (ListObjectsV2
// with delimiter "/"); a new folder is an empty "folder/" object, as in the AWS console.
//
// Downloads fetch 8 MB ranges, `parallelParts` at a time, and hand them on in file order (so a partial download is
// always a prefix of the file); every range is sent with If-Match: <ETag>, so a file that changes on the server
// meanwhile fails instead of mixing versions. Uploads below the part size are one PUT; bigger ones are multipart
// uploads with `parallelParts` parts in flight. An upload that fails (not one that is cancelled) is kept open, and
// uploading the same file again in this session continues it (ListParts). Requests run on the session's own pool of at
// most 16 threads. Rename / move is a server-side copy, then a delete of the original (folders key by key).
#include "sftp/RemoteFs.h"

#include <memory>

namespace st {
struct S3Connection;
}

namespace st::sftpimpl {

std::shared_ptr<RemoteFs> openS3Fs(std::shared_ptr<S3Connection> connection);

/** Tests: the high-water mark of bytes held in S3 reader and writer buffers since the last reset. */
uint64_t s3BufferedHighWater();
void resetS3BufferedHighWater();

}  // namespace st::sftpimpl
