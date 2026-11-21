#include "sftp/TransferEngine.h"

#include "core/Json.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "platform/ui/LocalFs.h"
#include "platform/sys/Errors.h"
#include "platform/sys/Fs.h"
#include "sftp/SftpSupport.h"
#include "sftp/SftpUtil.h"

#include <algorithm>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <vector>

namespace st::sftpimpl {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr auto kProgressInterval = std::chrono::milliseconds(100);
constexpr const char* kPartialSuffix = ".st-partial";
/** Next to a partial file: which version of the remote file it is the start of (remote path, size, time). */
constexpr const char* kPartialInfoSuffix = ".st-partial.info";

fs::path partialInfoPath(const fs::path& target) {
    fs::path p = target;
    p += kPartialInfoSuffix;
    return p;
}

/** The record for a download of `remote` (`size` bytes, modified `mtime`) into `target`'s partial file. */
std::string partialRecord(const std::string& remote, uint64_t size, int64_t mtime) {
    return Json{{"remote", remote}, {"size", size}, {"mtime", mtime}}.dump();
}

/** True when the record next to `target`'s partial file says it came from this version of the remote file. */
bool partialMatches(const fs::path& target, const std::string& remote, uint64_t size, int64_t mtime) {
    const auto text = sys::readFile(partialInfoPath(target));
    if (!text) return false;
    const Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object()) return false;
    return j.value("remote", std::string()) == remote && j.value("size", uint64_t{0}) == size &&
           j.value("mtime", int64_t{0}) == mtime;
}

void removePartialInfo(const fs::path& target) {
    std::string ignored;
    std::error_code ec;
    if (fs::exists(partialInfoPath(target), ec)) sys::removeFile(partialInfoPath(target), ignored);
}

std::string u8(const fs::path& p) { return sys::fromPath(p); }

/** Finder's folder settings (.DS_Store) and the "._name" files macOS writes for resource forks elsewhere. */
bool isMacFile(const fs::path& name) {
    const std::string n = sys::fromPath(name);
    return n == ".DS_Store" || n.rfind("._", 0) == 0;
}

/** Size / mtime (Unix s) / folder flag of a local path; false when it does not exist. */
bool localInfo(const fs::path& p, uint64_t& size, int64_t& mtime, bool& isDir) {
    const auto info = sys::fileInfo(p);
    if (!info) return false;
    isDir = info->isDir;
    size = info->size;
    mtime = info->mtime;
    return true;
}

/** One planned unit of a folder transfer. */
struct Item {
    bool dir = false;
    std::string remote;
    fs::path local;
    uint64_t size = 0;
    int64_t mtime = 0;  // Unix s (source side)
    std::string rel;    // shown as "current file"
    uint32_t mode = 0;  // permission bits of the source (downloads: the remote mode; uploads: 0 = 0644)
};

/** True when `path` is `root` or inside it ('/'-separated remote paths, or generic local ones). */
bool within(const std::string& path, const std::string& root) {
    if (path == root) return true;
    const std::string prefix = root.empty() || root.back() == '/' ? root : root + "/";
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

/** A folder walk (download or upload): the real paths of the folders on the way down, for links that lead back. */
struct WalkState {
    std::vector<std::string> chain;  // real path of the start folder, then of every folder below it on the way
    size_t entries = 0;              // checkTreeLimits
    int skippedLinks = 0;
    /** A link to a folder whose real path is in the tree being walked (or in a folder it came through): a loop or
     *  a second copy of something that is copied anyway. */
    bool leadsBack(const std::string& real) const {
        return std::any_of(chain.begin(), chain.end(), [&](const std::string& c) { return within(real, c); });
    }
};

struct PendingConflict {
    std::mutex m;
    std::condition_variable cv;
    std::optional<ConflictResolution> answer;
};

struct Queue {
    std::string id;
    std::shared_ptr<RemoteFs> fs;
    size_t parallel = 2;
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<TransferPtr> waiting;
    std::vector<TransferPtr> running;
    std::vector<std::thread> workers;
    size_t idle = 0;
    bool closing = false;
};
using QueuePtr = std::shared_ptr<Queue>;

}  // namespace

/* ------------------------------------------------------------------------ */
/* Pure helpers                                                              */
/* ------------------------------------------------------------------------ */

ConflictDecision decideConflict(const std::string& action, int64_t sourceMtime, int64_t targetMtime, bool canResume,
                                bool preciseTimes) {
    if (action == "skip") return ConflictDecision::Skip;
    if (action == "rename") return ConflictDecision::Rename;
    if (action == "resume") return canResume ? ConflictDecision::Resume : ConflictDecision::Overwrite;
    if (action == "newer") {
        if (sourceMtime <= 0 || targetMtime <= 0) return ConflictDecision::Overwrite;
        // FAT keeps 2 s; `ls` without full times only minutes.
        const int64_t tolerance = preciseTimes ? 2 : 60;
        return sourceMtime > targetMtime + tolerance ? ConflictDecision::Overwrite : ConflictDecision::Skip;
    }
    return ConflictDecision::Overwrite;
}

std::string numberedName(const std::string& name, int n) { return sftputil::numberedName(name, n); }

/* ------------------------------------------------------------------------ */
/* Engine                                                                    */
/* ------------------------------------------------------------------------ */

struct TransferEngine::Impl : std::enable_shared_from_this<TransferEngine::Impl> {
    EngineHooks hooks;
    std::atomic<bool> stopping{false};

    std::mutex m;
    std::unordered_map<std::string, QueuePtr> queues;
    std::unordered_map<std::string, TransferPtr> transfers;
    std::unordered_map<std::string, std::shared_ptr<PendingConflict>> conflicts;

    explicit Impl(EngineHooks h) : hooks(std::move(h)) {}

    /* ---------------------------------------------------------------- progress */

    void emit(const TransferProgress& p) {
        if (hooks.progress) hooks.progress(p);
    }

    template <class F>
    void update(const TransferPtr& t, bool force, F mutate) {
        TransferProgress copy;
        {
            std::lock_guard lk(t->pm);
            if (t->finished) return;
            mutate(t->progress);
            const auto now = std::chrono::steady_clock::now();
            if (!force && now - t->lastEmit < kProgressInterval) return;
            t->lastEmit = now;
            copy = t->progress;
        }
        emit(copy);
    }

    void setState(const TransferPtr& t, const std::string& state) {
        update(t, true, [&](TransferProgress& p) { p.state = state; });
    }
    void setTotals(const TransferPtr& t, uint64_t total, int files) {
        update(t, true, [&](TransferProgress& p) {
            p.total = static_cast<double>(total);
            p.filesTotal = files;
            p.listing = false;
            p.currentFile.clear();
        });
    }
    /** A folder tree being listed before the copy: what was found so far, and the folder being listed now. */
    void setListing(const TransferPtr& t, uint64_t total, int files, const std::string& folder, bool force) {
        update(t, force, [&](TransferProgress& p) {
            p.listing = true;
            p.total = static_cast<double>(total);
            p.filesTotal = files;
            p.currentFile = folder;
        });
    }
    void addProgress(const TransferPtr& t, uint64_t delta) {
        if (delta == 0) return;
        update(t, false, [&](TransferProgress& p) {
            p.transferred += static_cast<double>(delta);
            if (p.total < p.transferred) p.total = p.transferred;
        });
    }
    void setCurrent(const TransferPtr& t, const std::string& rel) {
        update(t, false, [&](TransferProgress& p) { p.currentFile = rel; });
    }
    void fileDone(const TransferPtr& t) {
        update(t, false, [&](TransferProgress& p) { ++p.filesDone; });
    }
    void fileSkipped(const TransferPtr& t, uint64_t size) {
        update(t, true, [&](TransferProgress& p) {
            ++p.skipped;
            ++p.filesDone;
            p.total = std::max(p.transferred, p.total - static_cast<double>(size));
        });
    }

    /**
     * One app-log line per transfer that ran: "sftp: download report.pdf done: 1 file, 845112 bytes in 0.4 s
     * (2.0 MiB/s)". For comparing speeds between versions and for bug reports; the name only, as in the other lines.
     */
    static void logFinished(const TransferProgress& p) {
        if (!p.startedAt || p.finishedAt < p.startedAt) return;  // cancelled while queued: never ran
        const double seconds = static_cast<double>(p.finishedAt - p.startedAt) / 1000.0;
        const auto bytes = static_cast<uint64_t>(p.transferred);
        const int files = p.filesTotal > 0 ? p.filesDone : (p.state == "done" ? 1 : 0);
        char secs[32];
        std::snprintf(secs, sizeof(secs), "%.1f", seconds);
        std::string line = "sftp: " + p.direction + " " + p.name + " " + p.state + ": " + std::to_string(files) +
                           (files == 1 ? " file, " : " files, ") + std::to_string(bytes) + " bytes in " + secs + " s";
        if (p.skipped) line += ", " + std::to_string(p.skipped) + " skipped";
        if (seconds > 0 && bytes) line += " (" + sftputil::humanSize(static_cast<uint64_t>(bytes / seconds)) + "/s)";
        applog::info(line);
    }

    /** Final state (exactly once). */
    void finish(const TransferPtr& t, const std::string& state, const std::string& error) {
        TransferProgress copy;
        std::function<void(bool, const std::string&)> onFinished;
        {
            std::lock_guard lk(t->pm);
            if (t->finished) return;
            t->finished = true;
            t->progress.state = state;
            t->progress.error = error;
            t->progress.finishedAt = nowMs();
            if (state == "done") t->progress.currentFile.clear();
            copy = t->progress;
            onFinished = std::move(t->onFinished);
            t->onFinished = nullptr;
        }
        logFinished(copy);
        emit(copy);
        {
            std::lock_guard lk(m);
            transfers.erase(t->id);
        }
        if (onFinished) onFinished(state == "done", state == "cancelled" ? std::string("Cancelled") : error);
    }

    void checkCancel(const TransferPtr& t) const {
        if (t->cancel || stopping || (t->group && t->group->cancelled)) throw Cancelled();
    }

    /* ---------------------------------------------------------------- queues */

    void enqueue(const TransferPtr& t) {
        QueuePtr q;
        {
            std::lock_guard lk(m);
            transfers[t->id] = t;
            auto it = queues.find(t->sessionId);
            if (it != queues.end()) q = it->second;
        }
        setState(t, "queued");
        if (!q) {
            t->cancel = true;
            finish(t, "cancelled", "");
            return;
        }
        std::lock_guard lk(q->qm);
        if (q->closing) {
            t->cancel = true;
            std::thread([self = shared_from_this(), t] { self->finish(t, "cancelled", ""); }).detach();
            return;
        }
        q->waiting.push_back(t);
        if (q->idle == 0 && q->workers.size() < q->parallel) {
            q->workers.emplace_back([self = shared_from_this(), q] { self->workerLoop(q); });
        } else {
            q->qcv.notify_one();
        }
    }

    void workerLoop(const QueuePtr& q) {
        for (;;) {
            TransferPtr t;
            {
                std::unique_lock lk(q->qm);
                ++q->idle;
                q->qcv.wait(lk, [&] { return q->closing || !q->waiting.empty(); });
                --q->idle;
                if (q->waiting.empty()) return;  // closing
                t = q->waiting.front();
                q->waiting.pop_front();
                q->running.push_back(t);
            }
            runTransfer(*q, t);
            {
                std::lock_guard lk(q->qm);
                q->running.erase(std::remove(q->running.begin(), q->running.end(), t), q->running.end());
            }
        }
    }

    void closeQueue(const QueuePtr& q) {
        std::vector<TransferPtr> dropped;
        {
            std::lock_guard lk(q->qm);
            q->closing = true;
            for (auto& t : q->waiting) dropped.push_back(t);
            q->waiting.clear();
            for (auto& t : q->running) t->cancel = true;
        }
        q->qcv.notify_all();
        for (auto& t : dropped) {
            t->cancel = true;
            finish(t, "cancelled", "");
        }
        wakeConflicts();
        std::vector<std::thread> workers;
        {
            std::lock_guard lk(q->qm);
            workers.swap(q->workers);
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }

    void cancel(const std::string& transferId) {
        TransferPtr t;
        {
            std::lock_guard lk(m);
            auto it = transfers.find(transferId);
            if (it == transfers.end()) return;
            t = it->second;
        }
        t->cancel = true;
        QueuePtr q;
        {
            std::lock_guard lk(m);
            auto it = queues.find(t->sessionId);
            if (it != queues.end()) q = it->second;
        }
        bool wasWaiting = false;
        if (q) {
            std::lock_guard lk(q->qm);
            auto it = std::find(q->waiting.begin(), q->waiting.end(), t);
            if (it != q->waiting.end()) {
                q->waiting.erase(it);
                wasWaiting = true;
            }
        }
        if (wasWaiting) finish(t, "cancelled", "");  // running transfers notice the flag between steps
        wakeConflicts();
    }

    /* ---------------------------------------------------------------- conflicts */

    void wakeConflicts() {
        std::vector<std::shared_ptr<PendingConflict>> all;
        {
            std::lock_guard lk(m);
            for (auto& [id, p] : conflicts) all.push_back(p);
        }
        for (auto& p : all) p->cv.notify_all();
    }

    void resolveConflict(const std::string& conflictId, const ConflictResolution& r) {
        std::shared_ptr<PendingConflict> p;
        {
            std::lock_guard lk(m);
            auto it = conflicts.find(conflictId);
            if (it == conflicts.end()) return;
            p = it->second;
        }
        {
            std::lock_guard lk(p->m);
            p->answer = r;
        }
        p->cv.notify_all();
    }

    /** Decision for one existing target; asks the UI when the policy is "ask" (or the edit copy changed). */
    ConflictDecision resolve(const TransferPtr& t, TransferConflict c, bool preciseTimes, std::string& newName) {
        ConflictGroup& g = *t->group;
        newName.clear();
        auto fromAnswer = [&](const ConflictResolution& r) {
            if (r.action == "cancel") {
                g.cancelled = true;
                throw Cancelled();
            }
            newName = r.newName;
            return decideConflict(r.action, c.source.mtime / 1000, c.target.mtime / 1000, c.canResume, preciseTimes);
        };
        auto forAll = [&]() -> std::optional<ConflictResolution> {
            std::lock_guard lk(g.m);
            if (!g.forAll) return std::nullopt;
            ConflictResolution r = *g.forAll;
            r.newName.clear();  // "rename all" means automatic names
            return r;
        };
        if (auto r = forAll()) return fromAnswer(*r);
        if (g.policy != "ask" && c.reason != "changed") {
            return decideConflict(g.policy, c.source.mtime / 1000, c.target.mtime / 1000, c.canResume, preciseTimes);
        }

        while (!g.askMutex.try_lock_for(200ms)) checkCancel(t);
        std::unique_lock ask(g.askMutex, std::adopt_lock);
        if (auto r = forAll()) return fromAnswer(*r);

        c.conflictId = newUuid();
        c.transferId = t->id;
        c.sftpId = t->sessionId;
        c.direction = t->kind == TransferKind::Upload ? "upload" : "download";
        c.multiple = g.multiple;
        auto pending = std::make_shared<PendingConflict>();
        {
            std::lock_guard lk(m);
            conflicts[c.conflictId] = pending;
        }
        auto unregister = [&] {
            std::lock_guard lk(m);
            conflicts.erase(c.conflictId);
        };
        if (hooks.conflict) hooks.conflict(c);
        ConflictResolution answer;
        try {
            std::unique_lock pl(pending->m);
            while (!pending->answer) {
                pending->cv.wait_for(pl, 200ms);
                if (pending->answer) break;
                pl.unlock();
                checkCancel(t);
                pl.lock();
            }
            answer = *pending->answer;
        } catch (...) {
            unregister();
            throw;
        }
        unregister();
        if (answer.applyToAll && answer.action != "cancel") {
            std::lock_guard lk(g.m);
            g.forAll = answer;
        }
        return fromAnswer(answer);
    }

    /* ---------------------------------------------------------------- running */

    void runTransfer(Queue& q, const TransferPtr& t) {
        std::string state = "done";
        std::string error;
        try {
            checkCancel(t);
            update(t, true, [](TransferProgress& p) {
                p.state = "running";
                if (!p.startedAt) p.startedAt = nowMs();
            });
            switch (t->kind) {
                case TransferKind::Download: runDownload(q, t); break;
                case TransferKind::Upload: runUpload(q, t); break;
                case TransferKind::DeleteRemote: runDeleteRemote(q, t); break;
                case TransferKind::DeleteLocal: runDeleteLocal(t); break;
            }
        } catch (const Cancelled&) {
            state = "cancelled";
        } catch (const std::exception& e) {
            state = (t->cancel || (t->group && t->group->cancelled)) ? "cancelled" : "error";
            error = e.what();
        }
        if (state == "error") applog::warn("sftp: transfer " + t->progress.name + " failed: " + error);
        finish(t, state, error);
    }

    /* ---- download */

    /**
     * Lists a remote folder for a download, depth first. Links to folders are followed (their content is copied as
     * a normal folder, as scp -r does), except those whose real path leads back into the tree (WalkState): those are
     * counted in skippedLinks. Past the tree limits (checkTreeLimits) the transfer fails before anything is copied.
     */
    void planDownloadDir(RemoteFs& rfs, const TransferPtr& t, WalkState& walk, const std::string& remoteDir,
                         const std::string& realDir, const fs::path& localDir, const std::string& rel, int depth,
                         std::vector<Item>& items, uint64_t& total, int& files, bool caseSensitive) {
        items.push_back(Item{true, remoteDir, localDir, 0, 0, rel});
        setListing(t, total, files, rel, false);
        walk.chain.push_back(realDir);
        sftputil::LocalNames names(caseSensitive);
        for (auto& [e, targetSize] : rfs.list(remoteDir, true, &t->cancel)) {
            checkCancel(t);
            const bool folderLink = e.type == "link" && e.linkTargetIsDir;
            checkTreeLimits(depth + (e.type == "dir" || folderLink ? 1 : 0), ++walk.entries, e.path);
            const std::string childRel = rel.empty() ? e.name : rel + "/" + e.name;
            const std::string wanted = sys::fromPath(sftputil::sanitizeLocalName(e.name));
            const std::string name = names.claim(wanted);
            if (name != wanted)
                applog::warn("sftp: " + childRel + " differs from another name only in letter case, which this disk "
                             "ignores: downloaded as " + name);
            const fs::path local = localDir / sys::toPath(name);
            if (e.type == "dir") {
                planDownloadDir(rfs, t, walk, e.path, sftputil::joinRemote(realDir, e.name), local, childRel, depth + 1,
                                items, total, files, caseSensitive);
            } else if (folderLink) {
                std::string real;
                try {
                    real = rfs.realpath(e.path);
                } catch (const ConnectionLost&) {
                    throw;
                } catch (const SftpError&) {
                }
                if (real.empty() || walk.leadsBack(real)) {
                    applog::info("sftp: " + childRel + " is a link to a folder " +
                                 (real.empty() ? std::string("that cannot be resolved") : "back in the tree (" + real + ")") +
                                 "; not followed");
                    ++walk.skippedLinks;
                    continue;
                }
                planDownloadDir(rfs, t, walk, e.path, real, local, childRel, depth + 1, items, total, files, caseSensitive);
            } else if (e.type == "file" || (e.type == "link" && targetSize > 0)) {
                const uint64_t size = e.type == "file" ? static_cast<uint64_t>(e.size) : targetSize;
                items.push_back(Item{false, e.path, local, size, e.mtime / 1000, childRel, e.mode});
                total += size;
                ++files;
            } else if (e.type == "link") {
                // Link to an empty file, or dangling: try it (an open error is reported).
                items.push_back(Item{false, e.path, local, 0, 0, childRel, e.mode});
                ++files;
            }
            // Special files (sockets, devices) are skipped.
        }
        walk.chain.pop_back();
    }

    void runDownload(Queue& q, const TransferPtr& t) {
        RemoteFs& rfs = *q.fs;
        // stat (follows links): a top-level link to a folder is downloaded as that folder.
        auto st = rfs.stat(t->remotePath, true);
        if (!st) throw SftpError("Cannot access " + quotePath(t->remotePath) + ": No such file or folder");
        std::vector<Item> items;
        uint64_t total = 0;
        WalkState walk;
        if (st->type == "dir") {
            int found = 0;
            setListing(t, 0, 0, "", true);
            std::string real = t->remotePath;
            try {
                real = rfs.realpath(t->remotePath);
            } catch (const ConnectionLost&) {
                throw;
            } catch (const SftpError&) {
            }
            planDownloadDir(rfs, t, walk, t->remotePath, real, t->localPath, "", 0, items, total, found,
                            sys::caseSensitiveFolder(t->localPath));
            if (walk.skippedLinks) update(t, false, [&](TransferProgress& p) { p.skippedLinks = walk.skippedLinks; });
        } else if (st->type == "file") {
            total = static_cast<uint64_t>(st->size);
            items.push_back(Item{false, t->remotePath, t->localPath, total, st->mtime / 1000, st->name, st->mode});
        } else {
            throw SftpError(quotePath(t->remotePath) + " is not a regular file or folder");
        }
        const int files = static_cast<int>(std::count_if(items.begin(), items.end(), [](const Item& i) { return !i.dir; }));
        if (files > 1) t->group->multiple = true;
        setTotals(t, total, files);
        bool anySkipped = false;
        for (const auto& item : items) {
            checkCancel(t);
            if (item.dir) {
                std::error_code ec;
                if (fs::exists(item.local, ec) && !fs::is_directory(item.local, ec)) {
                    throw SftpError("Cannot create folder '" + u8(item.local) + "': a file with that name exists");
                }
                fs::create_directories(item.local, ec);
                if (ec) throw SftpError("Cannot create folder '" + u8(item.local) + "': " + ec.message());
                continue;
            }
            setCurrent(t, item.rel);
            if (downloadFile(q, t, item)) {
                fileDone(t);
                if (t->options.move) rfs.unlink(item.remote);
            } else {
                anySkipped = true;
                fileSkipped(t, item.size);
            }
        }
        if (t->options.move && st->type == "dir" && !anySkipped) {
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                if (!it->dir) continue;
                try {
                    rfs.rmdir(it->remote);
                } catch (const SftpError&) {
                    // Not empty (unlisted special files, links to folders): leave it.
                }
            }
        }
    }

    static fs::path uniqueLocal(const fs::path& target) {
        const std::string name = u8(target.filename());
        for (int n = 1; n < 10000; ++n) {
            fs::path candidate = target.parent_path() / sys::toPath(numberedName(name, n));
            std::error_code ec;
            if (!fs::exists(candidate, ec)) return candidate;
        }
        throw SftpError("Cannot find a free name for '" + u8(target) + "'");
    }

    /** false = skipped (conflict policy). */
    bool downloadFile(Queue& q, const TransferPtr& t, const Item& item) {
        RemoteFs& rfs = *q.fs;
        const RemoteCapabilities caps = rfs.capabilities();
        fs::path target = item.local;
        bool resumeTarget = false;
        for (;;) {
            uint64_t tsize = 0;
            int64_t tmtime = 0;
            bool tdir = false;
            if (!localInfo(target, tsize, tmtime, tdir)) break;
            if (tdir) throw SftpError("Cannot download " + quotePath(item.remote) + ": '" + u8(target) + "' is a folder");
            TransferConflict c;
            c.name = u8(target.filename());
            c.source = FileStat{item.remote, static_cast<double>(item.size), item.mtime * 1000};
            c.target = FileStat{u8(target), static_cast<double>(tsize), tmtime * 1000};
            c.canResume = caps.resume && tsize > 0 && tsize < item.size;
            std::string newName;
            const ConflictDecision d = resolve(t, c, caps.preciseTimes, newName);
            if (d == ConflictDecision::Skip) return false;
            if (d == ConflictDecision::Overwrite) break;
            if (d == ConflictDecision::Resume) {
                resumeTarget = true;
                break;
            }
            target = newName.empty() ? uniqueLocal(target) : target.parent_path() / sftputil::sanitizeLocalName(newName);
        }

        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        fs::path part = target;
        part += kPartialSuffix;
        std::string moveError;
        if (resumeTarget && !sys::renameReplace(target, part, moveError)) {
            throw SftpError("Cannot resume '" + u8(target) + "': " + moveError);
        }
        uint64_t resumeFrom = 0;
        if (caps.resume) {
            uint64_t psize = 0;
            int64_t pmtime = 0;
            bool pdir = false;
            if (localInfo(part, psize, pmtime, pdir) && !pdir && psize > 0 && psize < item.size) {
                // An interrupted earlier attempt left a shorter partial file. Continue it by itself only when it is
                // the start of this version of the remote file (else the result would be the start of the old one
                // and the end of the new one); the Resume answer to a conflict is the user's own choice.
                if (resumeTarget || partialMatches(target, item.remote, item.size, item.mtime)) {
                    resumeFrom = psize;
                } else {
                    applog::info("sftp: the partial file of " + u8(target.filename()) +
                                 " is from another version of the file (or has no record of it); starting over");
                }
            }
        }
        if (caps.resume) {
            // Written before the first byte: an interrupted download leaves its partial file with the record.
            try {
                sys::writeFileAtomic(partialInfoPath(target), partialRecord(item.remote, item.size, item.mtime));
            } catch (const std::exception& e) {
                applog::warn("sftp: cannot write " + u8(partialInfoPath(target).filename()) + ": " + e.what());
            }
        }
        sys::File h;
        if (!h.open(part, resumeFrom ? sys::File::Mode::WriteKeep : sys::File::Mode::Write)) {
            throw SftpError("Cannot create '" + u8(target) + "': " + h.error());
        }
        if (resumeFrom && (!h.seek(resumeFrom) || !h.truncate())) {
            throw SftpError("Cannot resume '" + u8(target) + "': " + h.error());
        }
        std::unique_ptr<RemoteReader> reader;
        try {
            reader = rfs.openRead(item.remote, resumeFrom);
            addProgress(t, resumeFrom);
            for (;;) {
                checkCancel(t);
                bool done = false;
                auto data = reader->read(done);
                if (!h.writeAll(data->data(), data->size())) {
                    throw SftpError("Cannot write '" + u8(target) + "': " + h.error());
                }
                addProgress(t, data->size());
                if (done) break;
            }
            reader->close();
            const int64_t mtime = reader->mtime() > 0 ? reader->mtime() : item.mtime;
            reader.reset();
            if (t->options.preserveTimestamps && mtime > 0) h.setTimes(mtime);
            h.close();
            if (!sys::renameReplace(part, target, moveError)) {
                throw SftpError("Cannot replace '" + u8(target) + "': " + moveError);
            }
            removePartialInfo(target);
            // A script stays runnable (macOS; Windows has no such bit). Only the executable bits are taken.
            if (!sys::addExecutableBits(target, item.mode))
                applog::warn("sftp: cannot make " + u8(target.filename()) + " executable: " + sys::lastErrorMessage());
        } catch (const Cancelled&) {
            if (reader) reader->abort();
            h.close();
            if (resumeFrom == 0) {  // a fresh partial file is not wanted
                sys::removeFile(part, moveError);
                removePartialInfo(target);
            }
            throw;
        } catch (...) {
            if (reader) reader->abort();
            h.close();
            // Keep the partial file (and its record) so the next attempt continues it (when the protocol can).
            if (!caps.resume) {
                sys::removeFile(part, moveError);
                removePartialInfo(target);
            }
            throw;
        }
        return true;
    }

    /* ---- upload */

    void runUpload(Queue& q, const TransferPtr& t) {
        RemoteFs& rfs = *q.fs;
        uint64_t size = 0;
        int64_t mtime = 0;
        bool isDir = false;
        if (!localInfo(t->localPath, size, mtime, isDir)) {
            throw SftpError("Cannot read '" + u8(t->localPath) + "': " + sys::lastErrorMessage());
        }
        std::vector<Item> items;
        uint64_t total = 0;
        if (isDir) {
            items.push_back(Item{true, t->remotePath, t->localPath, 0, mtime, ""});
            int found = 0;
            setListing(t, 0, 0, "", true);
            WalkState walk;
            std::error_code ec;
            const fs::path real = fs::weakly_canonical(t->localPath, ec);
            planUploadDir(t, walk, t->localPath, ec ? t->localPath : real, t->remotePath, "", 0, items, total, found);
            if (walk.skippedLinks) update(t, false, [&](TransferProgress& p) { p.skippedLinks = walk.skippedLinks; });
        } else {
            items.push_back(Item{false, t->remotePath, t->localPath, size, mtime, u8(t->localPath.filename()),
                                 sys::permissionBits(t->localPath).value_or(0)});
            total = size;
        }
        const int files = static_cast<int>(std::count_if(items.begin(), items.end(), [](const Item& i) { return !i.dir; }));
        if (files > 1) t->group->multiple = true;
        setTotals(t, total, files);
        bool anySkipped = false;
        for (const auto& item : items) {
            checkCancel(t);
            if (item.dir) {
                ensureRemoteDir(rfs, item.remote);
                continue;
            }
            setCurrent(t, item.rel);
            if (uploadFile(q, t, item)) {
                fileDone(t);
                std::string deleteError;
                if (t->options.move && !sys::removeFile(item.local, deleteError)) {
                    throw SftpError("Uploaded, but cannot delete '" + u8(item.local) + "': " + deleteError);
                }
            } else {
                anySkipped = true;
                fileSkipped(t, item.size);
            }
        }
        if (t->options.move && isDir && !anySkipped) {
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                std::error_code ec;
                if (it->dir) fs::remove(it->local, ec);  // best effort: only empty folders go (hidden leftovers keep it)
            }
        }
    }

    /** The local side of planDownloadDir: folders depth first, links to folders followed unless they lead back. */
    void planUploadDir(const TransferPtr& t, WalkState& walk, const fs::path& dir, const fs::path& realDir,
                       const std::string& remoteDir, const std::string& rel, int depth, std::vector<Item>& items,
                       uint64_t& total, int& found) {
        walk.chain.push_back(sys::fromGenericPath(realDir));
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) throw SftpError("Cannot read folder '" + u8(dir) + "': " + ec.message());
        std::vector<fs::path> children;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            children.push_back(it->path());
        }
        std::sort(children.begin(), children.end());
        for (const fs::path& p : children) {
            checkCancel(t);
            const std::string name = u8(p.filename());
            const std::string childRel = rel.empty() ? name : rel + "/" + name;
            const std::string remote = sftputil::joinRemote(remoteDir, name);
            const auto info = sys::fileInfo(p);
            if (!info) continue;
            // A symbolic link, and on Windows also a junction (any reparse point): its real path decides.
            const bool link = info->isLink;
            const bool isDirectory = fs::is_directory(p, ec);  // follows links
            checkTreeLimits(depth + (isDirectory ? 1 : 0), ++walk.entries, u8(p));
            uint64_t sz = info->size;
            const int64_t mt = info->mtime;
            if (isDirectory) {
                fs::path real = realDir / p.filename();
                if (link) {
                    real = fs::canonical(p, ec);
                    if (ec || walk.leadsBack(sys::fromGenericPath(real))) {
                        applog::info("sftp: " + childRel + " is a link to a folder " +
                                     (ec ? std::string("that cannot be resolved") : "back in the tree (" + u8(real) + ")") +
                                     "; not followed");
                        ++walk.skippedLinks;
                        continue;
                    }
                }
                items.push_back(Item{true, remote, p, 0, mt, childRel});
                setListing(t, total, found, childRel, false);
                planUploadDir(t, walk, p, real, remote, childRel, depth + 1, items, total, found);
            } else if (t->options.skipMacFiles && isMacFile(p.filename())) {
                continue;
            } else if (fs::is_regular_file(p, ec)) {  // follows links: a link to a file is uploaded as its content
                if (link) sz = fs::file_size(p, ec);
                items.push_back(Item{false, remote, p, ec ? 0 : sz, mt, childRel, sys::permissionBits(p).value_or(0)});
                total += ec ? 0 : sz;
                ++found;
            }
        }
        walk.chain.pop_back();
    }

    static void ensureRemoteDir(RemoteFs& rfs, const std::string& path) {
        if (auto st = rfs.stat(path, true)) {
            if (st->type == "dir") return;
            throw SftpError("Cannot create folder " + quotePath(path) + ": a file with that name exists");
        }
        try {
            rfs.mkdir(path);
        } catch (const SftpError&) {
            // Another worker may have created it in the meantime.
            auto st = rfs.stat(path, true);
            if (!st || st->type != "dir") throw;
        }
    }

    static std::string uniqueRemote(RemoteFs& rfs, const std::string& target) {
        const std::string dir = sftputil::remoteParent(target);
        const std::string name = sftputil::remoteBaseName(target);
        for (int n = 1; n < 10000; ++n) {
            const std::string candidate = sftputil::joinRemote(dir, numberedName(name, n));
            if (!rfs.stat(candidate, false)) return candidate;
        }
        throw SftpError("Cannot find a free name for " + quotePath(target));
    }

    /** false = skipped (conflict policy). */
    bool uploadFile(Queue& q, const TransferPtr& t, const Item& item) {
        RemoteFs& rfs = *q.fs;
        const RemoteCapabilities caps = rfs.capabilities();
        std::string target = item.remote;
        uint64_t offset = 0;
        for (;;) {
            auto st = rfs.stat(target, true);
            if (!st) break;
            if (st->type == "dir") throw SftpError("Cannot upload to " + quotePath(target) + ": it is a folder");
            const int64_t remoteMtime = st->mtime / 1000;
            const bool changed = t->expectedRemoteMtime > 0 && remoteMtime != t->expectedRemoteMtime;
            if (t->expectedRemoteMtime > 0 && !changed) break;  // edit upload over its own, unchanged copy
            TransferConflict c;
            c.name = sftputil::remoteBaseName(target);
            c.reason = changed ? "changed" : "exists";
            c.source = FileStat{u8(item.local), static_cast<double>(item.size), item.mtime * 1000};
            c.target = FileStat{target, st->size, st->mtime};
            const uint64_t tsize = static_cast<uint64_t>(st->size);
            c.canResume = caps.resume && !changed && tsize > 0 && tsize < item.size;
            std::string newName;
            const ConflictDecision d = resolve(t, c, caps.preciseTimes, newName);
            if (d == ConflictDecision::Skip) return false;
            if (d == ConflictDecision::Overwrite) break;
            if (d == ConflictDecision::Resume) {
                offset = tsize;
                break;
            }
            target = newName.empty() ? uniqueRemote(rfs, target)
                                     : sftputil::joinRemote(sftputil::remoteParent(target), newName);
            t->expectedRemoteMtime = 0;
        }

        sys::File h;
        if (!h.open(item.local, sys::File::Mode::Read)) {
            throw SftpError("Cannot open '" + u8(item.local) + "': " + h.error());
        }
        if (offset && !h.seek(offset)) {
            throw SftpError("Cannot read '" + u8(item.local) + "': " + h.error());
        }
        WriteSpec spec;
        // A new remote file gets the local file's permission bits (macOS; Windows has none: 0644). An existing one
        // keeps its own (SFTP and scp apply the mode only when they create the file).
        spec.mode = item.mode ? item.mode : 0644;
        spec.offset = offset;
        spec.size = item.size;
        spec.mtime = t->options.preserveTimestamps ? item.mtime : 0;
        std::unique_ptr<RemoteWriter> w;
        try {
            w = rfs.openWrite(target, spec);
            addProgress(t, offset);
            const ProgressFn progress = [this, &t](uint64_t n) {
                checkCancel(t);
                addProgress(t, n);
            };
            for (bool eof = false; !eof;) {
                checkCancel(t);
                auto buf = std::make_shared<std::string>(kStepBytes, '\0');
                const std::int64_t n = h.read(buf->data(), buf->size());
                if (n < 0) throw SftpError("Cannot read '" + u8(item.local) + "': " + h.error());
                buf->resize(static_cast<size_t>(n));
                eof = n == 0;
                if (n) w->write(buf, progress);
            }
            w->finish(spec.mtime, progress);
            w.reset();
        } catch (const Cancelled&) {
            if (w) w->abort();
            if (offset == 0) {
                try {
                    rfs.unlink(target);  // remove the incomplete copy (best effort)
                } catch (const std::exception&) {
                }
            }
            throw;
        } catch (...) {
            if (w) w->abort();
            throw;
        }
        if (t->expectedRemoteMtime > 0) {
            if (auto st = rfs.stat(target, true)) t->expectedRemoteMtime = st->mtime / 1000;
        }
        return true;
    }

    /* ---- deletes (synchronize) */

    void runDeleteRemote(Queue& q, const TransferPtr& t) {
        setTotals(t, 0, 1);
        setCurrent(t, t->progress.name);
        if (q.fs->stat(t->remotePath, false)) q.fs->removeTree(t->remotePath, &t->cancel);
        fileDone(t);
    }

    void runDeleteLocal(const TransferPtr& t) {
        setTotals(t, 0, 1);
        setCurrent(t, t->progress.name);
        std::error_code ec;
        if (fs::exists(t->localPath, ec)) {
            ui::localfs::ShellOpOptions opts;
            opts.showUi = false;
            ui::localfs::remove({u8(t->localPath)}, t->options.deleteToRecycleBin, opts);
        }
        fileDone(t);
    }
};

TransferEngine::TransferEngine(EngineHooks hooks) : impl_(std::make_shared<Impl>(std::move(hooks))) {}

TransferEngine::~TransferEngine() { stop(); }

void TransferEngine::addQueue(const std::string& sessionId, std::shared_ptr<RemoteFs> rfs, int parallel) {
    auto q = std::make_shared<Queue>();
    q->id = sessionId;
    q->fs = std::move(rfs);
    q->parallel = static_cast<size_t>(std::clamp(parallel, 1, 6));
    std::lock_guard lk(impl_->m);
    impl_->queues[sessionId] = q;
}

void TransferEngine::removeQueue(const std::string& sessionId) {
    QueuePtr q;
    {
        std::lock_guard lk(impl_->m);
        auto it = impl_->queues.find(sessionId);
        if (it == impl_->queues.end()) return;
        q = it->second;
        impl_->queues.erase(it);
    }
    impl_->closeQueue(q);
}

std::shared_ptr<ConflictGroup> TransferEngine::newGroup(const TransferOptions& opts) {
    auto g = std::make_shared<ConflictGroup>();
    g->policy = opts.conflict.empty() ? "ask" : opts.conflict;
    return g;
}

TransferPtr TransferEngine::create(const std::string& sessionId, TransferKind kind, std::string remotePath,
                                   std::filesystem::path localPath, std::string name, const TransferOptions& opts,
                                   std::shared_ptr<ConflictGroup> group, std::string origin) {
    auto t = std::make_shared<Transfer>();
    t->id = newUuid();
    t->sessionId = sessionId;
    t->kind = kind;
    t->remotePath = std::move(remotePath);
    t->localPath = std::move(localPath);
    t->options = opts;
    t->group = group ? std::move(group) : newGroup(opts);
    t->progress.transferId = t->id;
    t->progress.sftpId = sessionId;
    t->progress.direction = kind == TransferKind::Upload ? "upload" : kind == TransferKind::Download ? "download" : "delete";
    t->progress.name = std::move(name);
    t->progress.localPath = sys::fromPath(t->localPath);
    t->progress.remotePath = t->remotePath;
    t->progress.state = "queued";
    t->progress.move = opts.move;
    t->progress.origin = std::move(origin);
    return t;
}

void TransferEngine::enqueue(const TransferPtr& t) { impl_->enqueue(t); }

void TransferEngine::cancel(const std::string& transferId) { impl_->cancel(transferId); }

void TransferEngine::resolveConflict(const std::string& conflictId, const ConflictResolution& resolution) {
    impl_->resolveConflict(conflictId, resolution);
}

void TransferEngine::stop() {
    if (!impl_ || impl_->stopping.exchange(true)) return;
    std::vector<QueuePtr> all;
    {
        std::lock_guard lk(impl_->m);
        for (auto& [id, q] : impl_->queues) all.push_back(q);
        impl_->queues.clear();
    }
    for (auto& q : all) impl_->closeQueue(q);
}

}  // namespace st::sftpimpl
