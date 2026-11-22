#include "sftp/RemoteFs.h"

#include "core/Log.h"

#include <mutex>

namespace st::sftpimpl {

namespace {

std::mutex g_limitsMu;
TreeLimits g_limits;

std::string withThousands(size_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

void removeTreeAt(RemoteFs& fs, const std::string& path, const std::atomic<bool>* cancel, int depth, size_t& entries) {
    for (const auto& e : fs.list(path, false, cancel)) {
        if (cancel && *cancel) throw Cancelled();
        checkTreeLimits(depth + (e.entry.type == "dir" ? 1 : 0), ++entries, e.entry.path);
        if (e.entry.type == "dir")
            removeTreeAt(fs, e.entry.path, cancel, depth + 1, entries);
        else
            fs.unlink(e.entry.path);
    }
    fs.rmdir(path);
}

}  // namespace

TreeLimits treeLimits() {
    std::lock_guard lock(g_limitsMu);
    return g_limits;
}

void setTreeLimitsForTests(TreeLimits limits) {
    std::lock_guard lock(g_limitsMu);
    g_limits = limits;
}

void checkTreeLimits(int depth, size_t entries, const std::string& where) {
    const TreeLimits l = treeLimits();
    std::string error;
    if (depth > l.depth)
        error = "The folder is nested too deeply (more than " + std::to_string(l.depth) + " levels)";
    else if (entries > l.entries)
        error = "The folder has more than " + withThousands(l.entries) + " files and folders";
    if (error.empty()) return;
    applog::warn("sftp: " + error + "; stopped at " + where);
    throw SftpError(error);
}

void RemoteFs::removeTree(const std::string& path, const std::atomic<bool>* cancel) {
    // lstat: a symlink to a folder is removed as a link, never followed.
    auto st = stat(path, false);
    if (!st) throw SftpError("Cannot delete " + quotePath(path) + ": No such file or folder");
    if (st->type != "dir") {
        unlink(path);
        return;
    }
    size_t entries = 0;
    removeTreeAt(*this, path, cancel, 0, entries);
}

std::string RemoteFs::presign(const std::string&, int64_t) {
    throw SftpError("Links to files are only available on S3");
}

}  // namespace st::sftpimpl
