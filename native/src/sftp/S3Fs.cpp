#include "sftp/S3Fs.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "s3/S3Client.h"
#include "s3/S3Transport.h"
#include "sftp/SftpUtil.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace st::sftpimpl {

namespace {

using s3::S3Client;
using s3::S3Error;

constexpr uint64_t kRangeBytes = 8ull << 20;
constexpr uint64_t kSingleCopyLimit = 5ull << 30;  // CopyObject's limit
constexpr uint64_t kCopyPartBytes = 512ull << 20;
constexpr int kMaxThreads = 16;
constexpr int kRangeAttempts = 5;
constexpr size_t kChunk = 256 * 1024;  // RemoteReader contract

/* ------------------------------------------------------------- buffered bytes (tests) */

std::atomic<uint64_t> g_buffered{0};
std::atomic<uint64_t> g_bufferedHigh{0};

void addBuffered(int64_t delta) {
    const uint64_t now = g_buffered.fetch_add(static_cast<uint64_t>(delta)) + static_cast<uint64_t>(delta);
    for (uint64_t high = g_bufferedHigh.load(); now > high && !g_bufferedHigh.compare_exchange_weak(high, now);) {
    }
}

/* ------------------------------------------------------------- pool */

/** At most `max` threads, started as work arrives, ended with the pool. */
class Pool {
public:
    explicit Pool(int max) : max_(max) {}
    ~Pool() { stop(); }

    void run(std::function<void()> job) {
        std::lock_guard lk(m_);
        if (stopping_) return;
        jobs_.push_back(std::move(job));
        if (idle_ == 0 && static_cast<int>(threads_.size()) < max_) threads_.emplace_back([this] { work(); });
        cv_.notify_one();
    }

    void stop() {
        std::vector<std::thread> ts;
        {
            std::lock_guard lk(m_);
            if (stopping_) return;
            stopping_ = true;
            ts.swap(threads_);
        }
        cv_.notify_all();
        for (auto& t : ts)
            if (t.joinable()) t.join();
    }

    int threads() const {
        std::lock_guard lk(m_);
        return static_cast<int>(threads_.size());
    }

private:
    void work() {
        std::unique_lock lk(m_);
        for (;;) {
            ++idle_;
            cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
            --idle_;
            if (jobs_.empty()) return;  // stopping
            auto job = std::move(jobs_.front());
            jobs_.pop_front();
            lk.unlock();
            try {
                job();
            } catch (...) {
            }
            lk.lock();
        }
    }

    const int max_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    int idle_ = 0;
    bool stopping_ = false;
};

/* ------------------------------------------------------------- paths */

struct Loc {
    bool root = false;  // the list of buckets
    std::string bucket;
    std::string key;  // no leading or trailing '/'; "" = the bucket's top
};

std::string normalizePath(const std::string& path) {
    std::vector<std::string> parts;
    for (const std::string& p : split(path, '/', true)) {
        if (p == ".") continue;
        if (p == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(p);
    }
    return "/" + join(parts, "/");
}

std::string contentTypeFor(const std::string& key) {
    static const std::map<std::string, std::string> kTypes = {
        {"txt", "text/plain"},         {"html", "text/html"},          {"htm", "text/html"},
        {"css", "text/css"},           {"js", "text/javascript"},      {"json", "application/json"},
        {"xml", "application/xml"},    {"csv", "text/csv"},            {"md", "text/markdown"},
        {"png", "image/png"},          {"jpg", "image/jpeg"},          {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},          {"svg", "image/svg+xml"},       {"webp", "image/webp"},
        {"ico", "image/x-icon"},       {"pdf", "application/pdf"},     {"zip", "application/zip"},
        {"gz", "application/gzip"},    {"tar", "application/x-tar"},   {"mp4", "video/mp4"},
        {"mp3", "audio/mpeg"},         {"wasm", "application/wasm"},   {"woff2", "font/woff2"},
    };
    const size_t dot = key.rfind('.');
    if (dot == std::string::npos || key.find('/', dot) != std::string::npos) return "application/octet-stream";
    auto it = kTypes.find(toLower(key.substr(dot + 1)));
    return it == kTypes.end() ? "application/octet-stream" : it->second;
}

int64_t metaMtimeSeconds(const s3::S3Head& h) {
    auto it = h.metadata.find("mtime");
    if (it == h.metadata.end()) return 0;
    try {
        return static_cast<int64_t>(std::stod(it->second));  // rclone writes fractional seconds
    } catch (...) {
        return 0;
    }
}

SftpEntry fileEntry(const std::string& name, const std::string& path, uint64_t size, int64_t mtimeMs) {
    SftpEntry e;
    e.name = name;
    e.path = path;
    e.type = "file";
    e.size = static_cast<double>(size);
    e.mtime = mtimeMs;
    return e;
}

SftpEntry dirEntry(const std::string& name, const std::string& path, int64_t mtimeMs = 0) {
    SftpEntry e;
    e.name = name;
    e.path = path;
    e.type = "dir";
    e.mtime = mtimeMs;
    return e;
}

/** An interrupted multipart upload that may be continued (same size, time and part size). */
struct OpenUpload {
    std::string uploadId;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t partSize = 0;
};

/* ------------------------------------------------------------- the file system */

class S3FsImpl final : public RemoteFs, public std::enable_shared_from_this<S3FsImpl> {
public:
    explicit S3FsImpl(std::shared_ptr<S3Connection> c)
        : conn_(std::move(c)), client_(conn_->client), pool_(std::make_shared<Pool>(kMaxThreads)) {}
    ~S3FsImpl() override { close(); }

    std::string protocol() const override { return "s3"; }
    RemoteCapabilities capabilities() const override {
        RemoteCapabilities c;
        c.resume = true;
        c.preciseTimes = false;
        c.resumeUploads = false;
        c.atomicWrites = true;
        c.chmod = false;
        c.symlinks = false;
        c.presign = true;
        c.rootIsBuckets = conn_->bucket.empty();
        return c;
    }
    bool lost() const override { return conn_->closed.load() || closed_.load(); }

    std::string home() override {
        if (!conn_->bucket.empty()) return normalizePath(conn_->startPath);
        return "/";
    }
    std::string realpath(const std::string& path) override { return normalizePath(path); }

    std::vector<RemoteEntry> list(const std::string& path, bool statLinks, const std::atomic<bool>* cancel) override {
        const std::string dir = normalizePath(path);
        const Loc l = locate(dir);
        std::vector<RemoteEntry> out;
        if (l.root) {
            for (const auto& b : guard([&] { return client_->listBuckets(cancel); }))
                out.push_back({dirEntry(b.name, sftputil::joinRemote(dir, b.name), b.createdMs), 0});
            return out;
        }
        const std::string prefix = l.key.empty() ? std::string() : l.key + "/";
        std::string token;
        std::vector<size_t> files;
        do {
            if (cancel && *cancel) throw Cancelled();
            const s3::S3List page = guard([&] { return client_->listObjects(l.bucket, prefix, "/", token, 1000, cancel); });
            for (const auto& p : page.prefixes) {
                std::string name = p.substr(prefix.size());
                if (!name.empty() && name.back() == '/') name.pop_back();
                if (name.empty() || name == "." || name == "..") continue;  // "a//b", unreachable as a path
                out.push_back({dirEntry(name, sftputil::joinRemote(dir, name)), 0});
            }
            for (const auto& o : page.objects) {
                if (o.key == prefix) continue;  // the folder's own marker object
                const std::string name = o.key.substr(prefix.size());
                if (name.empty() || name == "." || name == "..") continue;
                files.push_back(out.size());
                out.push_back({fileEntry(name, sftputil::joinRemote(dir, name), o.size, o.lastModifiedMs), o.size});
            }
            checkTreeLimits(0, out.size(), dir);
            token = page.truncated ? page.nextToken : std::string();
        } while (!token.empty());
        if (statLinks && !files.empty()) applyMetadataTimes(l.bucket, prefix, out, files, cancel);
        return out;
    }

    std::optional<SftpEntry> stat(const std::string& path, bool) override {
        const std::string p = normalizePath(path);
        const Loc l = locate(p);
        if (l.root) return dirEntry("/", "/");
        const std::string name = p == "/" ? std::string("/") : sftputil::remoteBaseName(p);
        if (l.key.empty()) {
            if (!conn_->bucket.empty()) return dirEntry(name, p);
            try {
                client_->bucketRegion(l.bucket);
                return dirEntry(name, p);
            } catch (const S3Error& e) {
                if (e.code == "NoSuchBucket") return std::nullopt;
                if (e.code == "AccessDenied") return dirEntry(name, p);
                throw translate(e);
            }
        }
        if (auto h = guard([&] { return client_->headObject(l.bucket, l.key); })) {
            const int64_t meta = metaMtimeSeconds(*h);
            return fileEntry(name, p, h->size, meta ? meta * 1000 : h->lastModifiedMs);
        }
        const s3::S3List probe = guard([&] { return client_->listObjects(l.bucket, l.key + "/", "/", "", 1); });
        if (!probe.objects.empty() || !probe.prefixes.empty()) return dirEntry(name, p);
        return std::nullopt;
    }

    void mkdir(const std::string& path, uint32_t) override {
        const Loc l = locate(normalizePath(path));
        if (l.root || l.key.empty()) throw SftpError("Buckets cannot be created here: create them in the provider's console");
        guard([&] { return client_->putObject(l.bucket, l.key + "/", std::make_shared<std::string>(), {}); });
    }

    void rename(const std::string& from, const std::string& to) override {
        const std::string fp = normalizePath(from), tp = normalizePath(to);
        const Loc a = locate(fp), b = locate(tp);
        if (a.root || b.root || a.key.empty() || b.key.empty()) throw SftpError("Buckets cannot be renamed or moved");
        if (fp == tp) return;
        if (startsWith(tp + "/", fp + "/")) throw SftpError("A folder cannot be moved into itself");
        if (auto h = guard([&] { return client_->headObject(a.bucket, a.key); })) {
            if (guard([&] { return client_->headObject(b.bucket, b.key); }))
                throw SftpError("Cannot rename " + quotePath(fp) + ": " + quotePath(tp) + " already exists");
            copyObject(a.bucket, a.key, b.bucket, b.key, *h, nullptr);
            guard([&] {
                client_->deleteObject(a.bucket, a.key);
                return 0;
            });
            return;
        }
        // A folder: every key under it, copied then deleted, several at once.
        std::vector<s3::S3Object> objects = listTree(a.bucket, a.key + "/", nullptr);
        if (objects.empty()) throw SftpError("Cannot rename " + quotePath(fp) + ": No such file or folder");
        const std::string fromPrefix = a.key + "/", toPrefix = b.key + "/";
        runParallel(objects.size(), [&](size_t i) {
            const s3::S3Object& o = objects[i];
            const std::string target = toPrefix + o.key.substr(fromPrefix.size());
            s3::S3Head h;
            h.size = o.size;
            h.storageClass = o.storageClass;
            copyObject(a.bucket, o.key, b.bucket, target, h, nullptr);
            client_->deleteObject(a.bucket, o.key);
        }, nullptr, "Moved");
    }

    void unlink(const std::string& path) override {
        const Loc l = locate(normalizePath(path));
        if (l.root || l.key.empty()) throw SftpError("Buckets cannot be deleted here");
        guard([&] {
            client_->deleteObject(l.bucket, l.key);
            return 0;
        });
    }

    void rmdir(const std::string& path) override {
        const Loc l = locate(normalizePath(path));
        if (l.root || l.key.empty()) throw SftpError("Buckets cannot be deleted here");
        const s3::S3List probe = guard([&] { return client_->listObjects(l.bucket, l.key + "/", "/", "", 2); });
        for (const auto& o : probe.objects)
            if (o.key != l.key + "/") throw SftpError("Cannot delete " + quotePath(path) + ": the folder is not empty");
        if (!probe.prefixes.empty()) throw SftpError("Cannot delete " + quotePath(path) + ": the folder is not empty");
        guard([&] {
            client_->deleteObject(l.bucket, l.key + "/");
            return 0;
        });
    }

    void removeTree(const std::string& path, const std::atomic<bool>* cancel) override {
        const std::string p = normalizePath(path);
        const Loc l = locate(p);
        if (l.root || l.key.empty()) throw SftpError("Buckets cannot be deleted here");
        if (guard([&] { return client_->headObject(l.bucket, l.key); })) {
            unlink(p);
            return;
        }
        std::vector<std::string> keys;
        std::vector<std::string> failed;
        size_t total = 0;
        auto flush = [&] {
            if (keys.empty()) return;
            auto f = guard([&] { return client_->deleteObjects(l.bucket, keys, cancel); });
            failed.insert(failed.end(), f.begin(), f.end());
            keys.clear();
        };
        const std::string prefix = l.key + "/";
        std::string token;
        do {
            if (cancel && *cancel) throw Cancelled();
            const s3::S3List page = guard([&] { return client_->listObjects(l.bucket, prefix, "", token, 1000, cancel); });
            for (const auto& o : page.objects) {
                keys.push_back(o.key);
                checkTreeLimits(0, ++total, p);
            }
            flush();
            token = page.truncated ? page.nextToken : std::string();
        } while (!token.empty());
        if (total == 0) throw SftpError("Cannot delete " + quotePath(p) + ": No such file or folder");
        if (!failed.empty())
            throw SftpError("Could not delete " + std::to_string(failed.size()) + " of " + std::to_string(total) +
                            " objects; the first: " + failed.front());
    }

    void chmod(const std::string&, uint32_t) override { throw SftpError("Permissions cannot be changed on S3"); }
    void setMtime(const std::string&, int64_t) override {}  // kept in x-amz-meta-mtime when uploading

    std::unique_ptr<RemoteReader> openRead(const std::string& path, uint64_t offset) override;
    std::unique_ptr<RemoteWriter> openWrite(const std::string& path, const WriteSpec& spec) override;

    std::string presign(const std::string& path, int64_t seconds) override {
        const Loc l = locate(normalizePath(path));
        if (l.root || l.key.empty()) throw SftpError("Links can only be made to files");
        return guard([&] { return client_->presignGet(l.bucket, l.key, seconds); });
    }

    void close() override {
        if (closed_.exchange(true)) return;
        std::map<std::string, OpenUpload> left;
        {
            std::lock_guard lk(m_);
            left.swap(openUploads_);
        }
        // Interrupted uploads cost storage until aborted: abort them when the session ends.
        if (!conn_->closed) {
            for (const auto& [where, u] : left) {
                const size_t slash = where.find('/');
                try {
                    client_->abortMultipartUpload(where.substr(0, slash), where.substr(slash + 1), u.uploadId);
                } catch (const std::exception&) {
                }
            }
        }
        pool_->stop();
    }

    /* ---- shared with the reader and writer */

    Loc locate(const std::string& p) const {
        Loc l;
        std::string rest = p.size() > 1 ? p.substr(1) : std::string();
        if (!conn_->bucket.empty()) {
            l.bucket = conn_->bucket;
            l.key = rest;
            return l;
        }
        if (rest.empty()) {
            l.root = true;
            return l;
        }
        const size_t slash = rest.find('/');
        l.bucket = rest.substr(0, slash);
        l.key = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
        return l;
    }

    template <class F>
    auto guard(F f) -> decltype(f()) {
        if (lost()) throw ConnectionLost();
        try {
            return f();
        } catch (const S3Error& e) {
            throw translate(e);
        }
    }

    std::exception_ptr translateToPtr(const S3Error& e) const {
        if (lost()) return std::make_exception_ptr(ConnectionLost());
        if (e.cancelled) return std::make_exception_ptr(Cancelled());
        return std::make_exception_ptr(SftpError(e.what()));
    }

    SftpError translate(const S3Error& e) const {
        if (lost()) return ConnectionLost();
        return SftpError(e.what());
    }

    std::shared_ptr<S3Client> client() const { return client_; }
    std::shared_ptr<Pool> pool() const { return pool_; }
    const S3Connection& connection() const { return *conn_; }

    void rememberUpload(const std::string& where, OpenUpload u) {
        std::lock_guard lk(m_);
        openUploads_[where] = std::move(u);
    }
    std::optional<OpenUpload> takeUpload(const std::string& where) {
        std::lock_guard lk(m_);
        auto it = openUploads_.find(where);
        if (it == openUploads_.end()) return std::nullopt;
        OpenUpload u = it->second;
        openUploads_.erase(it);
        return u;
    }

private:
    void copyObject(const std::string& sb, const std::string& sk, const std::string& db, const std::string& dk,
                    const s3::S3Head& h, const std::atomic<bool>* cancel) {
        if (h.size <= kSingleCopyLimit) {
            guard([&] {
                client_->copyObject(sb, sk, db, dk, h.storageClass, nullptr, cancel);
                return 0;
            });
            return;
        }
        // Above 5 GB: a multipart copy, parts copied by the service itself.
        auto full = guard([&] { return client_->headObject(sb, sk); });
        if (!full) throw SftpError("Cannot copy " + quotePath(sk) + ": it is gone");
        s3::S3PutOptions opts;
        opts.contentType = full->contentType;
        opts.metadata = full->metadata;
        opts.storageClass = full->storageClass;
        const std::string id = guard([&] { return client_->createMultipartUpload(db, dk, opts); });
        const size_t n = static_cast<size_t>((full->size + kCopyPartBytes - 1) / kCopyPartBytes);
        std::vector<s3::S3Part> parts(n);
        try {
            runParallel(n, [&](size_t i) {
                const uint64_t first = i * kCopyPartBytes;
                const uint64_t last = std::min(full->size, first + kCopyPartBytes) - 1;
                parts[i] = {static_cast<int>(i + 1), client_->uploadPartCopy(sb, sk, db, dk, id, static_cast<int>(i + 1), first, last, cancel), 0};
            }, cancel, "");
            guard([&] {
                client_->completeMultipartUpload(db, dk, id, parts);
                return 0;
            });
        } catch (...) {
            try {
                client_->abortMultipartUpload(db, dk, id);
            } catch (const std::exception&) {
            }
            throw;
        }
    }

    /** Runs job(0 .. n-1) on the pool, parallelParts * 2 at a time; the first error is thrown after all finished. */
    void runParallel(size_t n, const std::function<void(size_t)>& job, const std::atomic<bool>* cancel, const std::string& verb) {
        if (n == 0) return;
        std::mutex m;
        std::condition_variable cv;
        size_t next = 0, done = 0, running = 0;
        std::exception_ptr first;
        std::atomic<bool> stop{false};
        const size_t width = static_cast<size_t>(std::max(1, conn_->parallelParts * 2));
        auto lastLog = std::chrono::steady_clock::now();
        std::unique_lock lk(m);
        while (done < n) {
            while (next < n && running < width && !stop) {
                const size_t i = next++;
                ++running;
                pool_->run([&, i] {
                    std::exception_ptr err;
                    try {
                        if (stop || (cancel && *cancel)) throw Cancelled();
                        job(i);
                    } catch (const S3Error& e) {
                        err = translateToPtr(e);
                    } catch (...) {
                        err = std::current_exception();
                    }
                    std::lock_guard g(m);
                    if (err && !first) {
                        first = err;
                        stop = true;
                    }
                    --running;
                    ++done;
                    cv.notify_all();
                });
            }
            if (stop && running == 0) break;
            cv.wait_for(lk, std::chrono::milliseconds(200));
            if (cancel && *cancel) stop = true;
            if (!verb.empty() && n > 50 && std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5)) {
                lastLog = std::chrono::steady_clock::now();
                if (conn_->log) conn_->log(LogLevel::Info, "S3: " + verb + " " + std::to_string(done) + " of " + std::to_string(n));
            }
        }
        while (running > 0) cv.wait(lk);
        if (first) std::rethrow_exception(first);
        if (cancel && *cancel) throw Cancelled();
    }

    std::vector<s3::S3Object> listTree(const std::string& bucket, const std::string& prefix, const std::atomic<bool>* cancel) {
        std::vector<s3::S3Object> out;
        std::string token;
        do {
            if (cancel && *cancel) throw Cancelled();
            s3::S3List page = guard([&] { return client_->listObjects(bucket, prefix, "", token, 1000, cancel); });
            for (auto& o : page.objects) {
                out.push_back(std::move(o));
                checkTreeLimits(0, out.size(), "/" + bucket + "/" + prefix);
            }
            token = page.truncated ? page.nextToken : std::string();
        } while (!token.empty());
        return out;
    }

    /** Folder walks (synchronize): the time a file was uploaded with (x-amz-meta-mtime) instead of LastModified. */
    void applyMetadataTimes(const std::string& bucket, const std::string& prefix, std::vector<RemoteEntry>& out,
                            const std::vector<size_t>& files, const std::atomic<bool>* cancel) {
        runParallel(files.size(), [&](size_t i) {
            RemoteEntry& e = out[files[i]];
            if (auto h = client_->headObject(bucket, prefix + e.entry.name, cancel))
                if (const int64_t meta = metaMtimeSeconds(*h)) e.entry.mtime = meta * 1000;
        }, cancel, "");
    }

    std::shared_ptr<S3Connection> conn_;
    std::shared_ptr<S3Client> client_;
    std::shared_ptr<Pool> pool_;
    std::atomic<bool> closed_{false};
    std::mutex m_;
    std::map<std::string, OpenUpload> openUploads_;
};

/* ------------------------------------------------------------- reading */

class S3Reader final : public RemoteReader {
public:
    S3Reader(std::shared_ptr<S3FsImpl> fs, Loc loc, const s3::S3Head& head, uint64_t offset)
        : st_(std::make_shared<State>()) {
        st_->fs = std::move(fs);
        st_->loc = std::move(loc);
        st_->size = head.size;
        st_->etag = head.etag;
        st_->pos = std::min(offset, head.size);
        st_->next = st_->pos;
        st_->width = static_cast<size_t>(std::max(1, st_->fs->connection().parallelParts));
        const int64_t meta = metaMtimeSeconds(head);
        mtime_ = meta ? meta : head.lastModifiedMs / 1000;
    }
    ~S3Reader() override { abort(); }

    uint64_t size() const override { return st_->size; }
    int64_t mtime() const override { return mtime_; }

    std::shared_ptr<std::string> read(bool& done) override {
        std::unique_lock lk(st_->m);
        done = false;
        if (st_->pos >= st_->size) {
            done = true;
            return std::make_shared<std::string>();
        }
        schedule(lk);
        const auto front = st_->ranges.front();
        st_->cv.wait(lk, [&] {
            return st_->cancel || front->failed || front->data.size() > front->consumed || front->done;
        });
        if (st_->cancel) throw Cancelled();
        if (front->failed) std::rethrow_exception(front->error);
        const size_t n = std::min(kChunk, front->data.size() - front->consumed);
        auto out = std::make_shared<std::string>(front->data, front->consumed, n);
        front->consumed += n;
        st_->pos += n;
        // Freed once all of it is handed on, even if its request has not quite returned yet.
        if (front->consumed == front->length) {
            addBuffered(-static_cast<int64_t>(front->data.size()));
            front->data = std::string();
            st_->ranges.pop_front();
            schedule(lk);
        }
        done = st_->pos >= st_->size;
        return out;
    }

    void close() override {
        std::lock_guard lk(st_->m);
        for (const auto& r : st_->ranges)
            if (r->failed) std::rethrow_exception(r->error);
    }

    void abort() noexcept override {
        {
            std::lock_guard lk(st_->m);
            if (st_->cancel) return;
            st_->cancel = true;
        }
        st_->cv.notify_all();
    }

private:
    struct Range {
        uint64_t offset = 0, length = 0;
        uint64_t received = 0;  // what arrived; `data` is emptied once it is all handed on
        std::string data;
        size_t consumed = 0;
        bool done = false, failed = false;
        std::exception_ptr error;
    };
    struct State {
        std::shared_ptr<S3FsImpl> fs;
        Loc loc;
        uint64_t size = 0, pos = 0, next = 0;
        std::string etag;
        size_t width = 4;
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::shared_ptr<Range>> ranges;
        std::atomic<bool> cancel{false};
        ~State() {
            for (const auto& r : ranges) addBuffered(-static_cast<int64_t>(r->data.size()));
        }
    };

    /** Keeps `width` ranges fetched ahead (under the lock). */
    void schedule(std::unique_lock<std::mutex>&) {
        while (st_->ranges.size() < st_->width && st_->next < st_->size) {
            auto r = std::make_shared<Range>();
            r->offset = st_->next;
            r->length = std::min(kRangeBytes, st_->size - st_->next);
            st_->next += r->length;
            st_->ranges.push_back(r);
            std::shared_ptr<State> st = st_;
            st_->fs->pool()->run([st, r] { fetch(st, r); });
        }
    }

    static void fetch(const std::shared_ptr<State>& st, const std::shared_ptr<Range>& r) {
        std::exception_ptr error;
        for (int attempt = 1;; ++attempt) {
            uint64_t got;
            {
                std::lock_guard lk(st->m);
                got = r->received;
            }
            if (attempt > 1 && got == r->length) break;  // it all arrived before the connection went
            uint64_t delivered = 0;
            try {
                st->fs->client()->getObject(st->loc.bucket, st->loc.key, r->offset + got, r->length - got, st->etag,
                                            [&](std::string_view chunk) {
                                                {
                                                    // Counted under the lock: read() uncounts under it too.
                                                    std::lock_guard lk(st->m);
                                                    if (st->cancel) return false;
                                                    r->data.append(chunk);
                                                    r->received += chunk.size();
                                                    addBuffered(static_cast<int64_t>(chunk.size()));
                                                }
                                                st->cv.notify_all();
                                                return true;
                                            },
                                            delivered, &st->cancel);
                std::lock_guard lk(st->m);
                if (r->received != r->length && !st->cancel)
                    throw SftpError("The server sent " + std::to_string(r->received) + " of " + std::to_string(r->length) + " bytes");
                break;
            } catch (const S3Error& e) {
                if (e.cancelled || st->cancel) {
                    error = std::make_exception_ptr(Cancelled());
                    break;
                }
                if (e.network && attempt < kRangeAttempts && !st->fs->lost()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200 << attempt));
                    continue;  // continue from what arrived
                }
                error = st->fs->translateToPtr(e);
                break;
            } catch (...) {
                error = std::current_exception();
                break;
            }
        }
        {
            std::lock_guard lk(st->m);
            r->done = true;
            if (error) {
                r->failed = true;
                r->error = error;
            }
        }
        st->cv.notify_all();
    }

    std::shared_ptr<State> st_;
    int64_t mtime_ = 0;
};

/* ------------------------------------------------------------- writing */

class S3Writer final : public RemoteWriter {
public:
    S3Writer(std::shared_ptr<S3FsImpl> fs, Loc loc, const WriteSpec& spec) : st_(std::make_shared<State>()) {
        st_->fs = std::move(fs);
        st_->loc = std::move(loc);
        st_->spec = spec;
        st_->partSize = std::max<uint64_t>(st_->fs->connection().partSize, 5ull << 20);
        // S3 allows 10,000 parts: a bigger file gets bigger parts.
        while (spec.size / st_->partSize >= 9999) st_->partSize *= 2;
        st_->width = static_cast<size_t>(std::max(1, st_->fs->connection().parallelParts));
        st_->opts.contentType = contentTypeFor(st_->loc.key);
        if (spec.mtime > 0) st_->opts.metadata["mtime"] = std::to_string(spec.mtime);
        st_->where = st_->loc.bucket + "/" + st_->loc.key;
        if (auto open = st_->fs->takeUpload(st_->where)) resumeFrom(*open);
    }
    ~S3Writer() override {
        if (!finished_) abort();
        addBuffered(-static_cast<int64_t>(buffer_.size()));
    }

    void write(const std::shared_ptr<std::string>& data, const ProgressFn& progress) override {
        buffer_ += *data;
        addBuffered(static_cast<int64_t>(data->size()));
        while (buffer_.size() >= st_->partSize) {
            auto part = std::make_shared<std::string>(buffer_.substr(0, st_->partSize));
            buffer_.erase(0, st_->partSize);
            submit(std::move(part), progress);
        }
        report(progress);
    }

    void finish(int64_t, const ProgressFn& progress) override {
        if (st_->uploadId.empty() && st_->nextPart == 1) {
            // Small file: one PUT, on the pool so that progress and cancel are handled here.
            auto body = std::make_shared<std::string>(std::move(buffer_));
            buffer_.clear();
            std::shared_ptr<State> st = st_;
            {
                std::lock_guard lk(st->m);
                ++st->inflight;
            }
            st->fs->pool()->run([st, body] {
                std::exception_ptr err;
                try {
                    st->fs->client()->putObject(st->loc.bucket, st->loc.key, body, st->opts,
                                                [&](uint64_t sent) { st->ack(0, sent); }, &st->cancel);
                } catch (const S3Error& e) {
                    err = st->fs->translateToPtr(e);
                } catch (...) {
                    err = std::current_exception();
                }
                addBuffered(-static_cast<int64_t>(body->size()));
                st->partDone(err);
            });
            waitAll(progress);
            finished_ = true;
            return;
        }
        if (!buffer_.empty() || st_->nextPart == 1) {
            auto part = std::make_shared<std::string>(std::move(buffer_));
            buffer_.clear();
            submit(std::move(part), progress);
        }
        waitAll(progress);
        std::vector<s3::S3Part> parts;
        {
            std::lock_guard lk(st_->m);
            for (const auto& [n, etag] : st_->etags) parts.push_back({n, etag, 0});
        }
        st_->fs->guard([&] {
            st_->fs->client()->completeMultipartUpload(st_->loc.bucket, st_->loc.key, st_->uploadId, parts);
            return 0;
        });
        finished_ = true;
    }

    void abort() noexcept override {
        if (finished_) return;
        finished_ = true;
        st_->cancel = true;
        st_->cv.notify_all();
        {
            std::unique_lock lk(st_->m);
            st_->cv.wait(lk, [&] { return st_->inflight == 0; });
        }
        if (st_->uploadId.empty()) return;
        // A failed upload (the network, the service) stays open so that uploading again continues it; a cancelled
        // one (the user) is aborted.
        if (st_->failed && !userCancelled_ && !st_->fs->lost()) {
            st_->fs->rememberUpload(st_->where, OpenUpload{st_->uploadId, st_->spec.size, st_->spec.mtime, st_->partSize});
            if (st_->fs->connection().log)
                st_->fs->connection().log(LogLevel::Info, "S3: the upload of " + st_->loc.key + " can be continued (" +
                                                              std::to_string(st_->etags.size()) + " parts sent)");
            return;
        }
        try {
            st_->fs->client()->abortMultipartUpload(st_->loc.bucket, st_->loc.key, st_->uploadId);
        } catch (const std::exception&) {
        }
    }

private:
    struct State {
        std::shared_ptr<S3FsImpl> fs;
        Loc loc;
        WriteSpec spec;
        s3::S3PutOptions opts;
        std::string where;
        uint64_t partSize = 16ull << 20;
        size_t width = 4;
        std::string uploadId;
        int nextPart = 1;
        std::map<int, uint64_t> alreadyThere;  // resuming: part number -> size on the server
        std::mutex m;
        std::condition_variable cv;
        std::map<int, std::string> etags;
        std::map<int, uint64_t> sent;  // bytes acknowledged per part (the most seen: a retry starts again)
        uint64_t acked = 0;
        size_t inflight = 0;
        bool failed = false;
        std::exception_ptr error;
        std::atomic<bool> cancel{false};

        void ack(int part, uint64_t bytes) {
            std::lock_guard lk(m);
            uint64_t& s = sent[part];
            if (bytes > s) {
                acked += bytes - s;
                s = bytes;
            }
        }
        void partDone(std::exception_ptr err) {
            {
                std::lock_guard lk(m);
                --inflight;
                if (err && !error) {
                    error = err;
                    failed = true;
                }
            }
            cv.notify_all();
        }
    };

    void resumeFrom(const OpenUpload& u) {
        if (u.size != st_->spec.size || u.mtime != st_->spec.mtime || u.partSize != st_->partSize) {
            try {
                st_->fs->client()->abortMultipartUpload(st_->loc.bucket, st_->loc.key, u.uploadId);
            } catch (const std::exception&) {
            }
            return;
        }
        try {
            for (const auto& p : st_->fs->client()->listParts(st_->loc.bucket, st_->loc.key, u.uploadId)) {
                st_->alreadyThere[p.number] = p.size;
                st_->etags[p.number] = p.etag;
            }
            st_->uploadId = u.uploadId;
            if (st_->fs->connection().log)
                st_->fs->connection().log(LogLevel::Info, "S3: continuing the upload of " + st_->loc.key + " (" +
                                                              std::to_string(st_->alreadyThere.size()) + " parts already there)");
        } catch (const std::exception&) {
            st_->alreadyThere.clear();
            st_->etags.clear();
        }
    }

    /** Engine thread: progress, cancel (Cancelled from `progress`) and the first error of a part. */
    void report(const ProgressFn& progress) {
        uint64_t acked;
        std::exception_ptr err;
        {
            std::lock_guard lk(st_->m);
            acked = st_->acked;
            err = st_->error;
        }
        if (err) std::rethrow_exception(err);
        if (acked > reported_) {
            const uint64_t delta = acked - reported_;
            reported_ = acked;
            try {
                progress(delta);
            } catch (const Cancelled&) {
                userCancelled_ = true;
                throw;
            }
        }
    }

    void submit(std::shared_ptr<std::string> part, const ProgressFn& progress) {
        // The part's bytes stay counted until the pool task is done with them; every other way out of here (sent
        // before, an error or a cancel thrown on the way) uncounts them now.
        struct Uncount {
            std::shared_ptr<std::string>& part;
            ~Uncount() {
                if (part) addBuffered(-static_cast<int64_t>(part->size()));
            }
        } uncount{part};
        if (st_->uploadId.empty())
            st_->uploadId = st_->fs->guard([&] { return st_->fs->client()->createMultipartUpload(st_->loc.bucket, st_->loc.key, st_->opts); });
        const int n = st_->nextPart++;
        if (n > 10000) throw SftpError("The file needs more than 10,000 parts");
        if (auto there = st_->alreadyThere.find(n); there != st_->alreadyThere.end() && there->second == part->size()) {
            st_->ack(n, part->size());  // sent before the interruption
            report(progress);
            return;
        }
        // Wait for room (bounded memory: `width` parts in flight).
        for (;;) {
            report(progress);
            std::unique_lock lk(st_->m);
            if (st_->inflight < st_->width) {
                ++st_->inflight;
                break;
            }
            st_->cv.wait_for(lk, std::chrono::milliseconds(100));
        }
        std::shared_ptr<State> st = st_;
        st->fs->pool()->run([st, part, n] {
            std::exception_ptr err;
            try {
                const std::string etag = st->fs->client()->uploadPart(st->loc.bucket, st->loc.key, st->uploadId, n, part,
                                                                       [&](uint64_t sent) { st->ack(n, sent); }, &st->cancel);
                st->ack(n, part->size());
                std::lock_guard lk(st->m);
                st->etags[n] = etag;
            } catch (const S3Error& e) {
                err = st->fs->translateToPtr(e);
            } catch (...) {
                err = std::current_exception();
            }
            addBuffered(-static_cast<int64_t>(part->size()));
            st->partDone(err);
        });
        part.reset();  // the task has it now
    }

    void waitAll(const ProgressFn& progress) {
        for (;;) {
            report(progress);
            std::unique_lock lk(st_->m);
            if (st_->inflight == 0) break;
            st_->cv.wait_for(lk, std::chrono::milliseconds(100));
        }
        report(progress);
    }

    std::shared_ptr<State> st_;
    std::string buffer_;
    uint64_t reported_ = 0;
    bool finished_ = false;
    bool userCancelled_ = false;
};

std::unique_ptr<RemoteReader> S3FsImpl::openRead(const std::string& path, uint64_t offset) {
    const std::string p = normalizePath(path);
    const Loc l = locate(p);
    if (l.root || l.key.empty()) throw SftpError("Cannot read " + quotePath(p) + ": it is a folder");
    auto h = guard([&] { return client_->headObject(l.bucket, l.key); });
    if (!h) throw SftpError("Cannot read " + quotePath(p) + ": No such file");
    if (h->storageClass == "GLACIER" || h->storageClass == "DEEP_ARCHIVE")
        throw SftpError("Cannot read " + quotePath(p) + ": it is archived (" + h->storageClass +
                        "); restore it in the provider's console first");
    return std::make_unique<S3Reader>(shared_from_this(), l, *h, offset);
}

std::unique_ptr<RemoteWriter> S3FsImpl::openWrite(const std::string& path, const WriteSpec& spec) {
    const std::string p = normalizePath(path);
    const Loc l = locate(p);
    if (l.root || l.key.empty()) throw SftpError("Files can only be uploaded into a bucket");
    if (spec.offset > 0) throw SftpError("S3 cannot add to an existing object");
    return std::make_unique<S3Writer>(shared_from_this(), l, spec);
}

}  // namespace

std::shared_ptr<RemoteFs> openS3Fs(std::shared_ptr<S3Connection> connection) {
    if (!connection || !connection->client) throw ConnectionLost();
    return std::make_shared<S3FsImpl>(std::move(connection));
}

uint64_t s3BufferedHighWater() { return g_bufferedHigh.load(); }
void resetS3BufferedHighWater() { g_bufferedHigh = g_buffered.load(); }

}  // namespace st::sftpimpl
