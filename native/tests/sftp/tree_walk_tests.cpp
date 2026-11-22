// Remote tree walks against a server that misbehaves or has links: the limits on nesting and entries (download,
// delete, find, synchronize), links to folders (followed unless they lead back into the tree, counted when not),
// and the executable bit in both directions. An in-memory RemoteFs stands in for the server: the test SFTP server has
// no links, and Windows cannot create them without Developer Mode.
#include "TestBridge.h"
#include "TestExecProcess.h"
#include "TestServers.h"

#include "platform/Platform.h"
#include "platform/sys/Fs.h"
#include "sftp/FindSync.h"
#include "sftp/SftpUtil.h"
#include "sftp/TransferEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>

using namespace st;
using namespace st::test;
using namespace st::sftpimpl;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/** A remote file system in memory. "/endless" lists one more subfolder "d" at every level, without end. */
class FakeFs final : public RemoteFs {
public:
    struct Node {
        std::string type;  // "dir" | "file" | "link"
        std::string content;
        uint32_t mode = 0644;
        std::string target;  // links: absolute
    };
    std::map<std::string, Node> nodes{{"/", Node{"dir", "", 0644, ""}}};
    std::map<std::string, uint32_t> createdModes;  // openWrite: the mode a new file got
    mutable std::mutex mu;

    void dir(const std::string& p) { nodes[p] = Node{"dir", "", 0755, ""}; }
    void file(const std::string& p, const std::string& content, uint32_t mode = 0644) { nodes[p] = Node{"file", content, mode, ""}; }
    void link(const std::string& p, const std::string& target) { nodes[p] = Node{"link", "", 0777, target}; }

    std::string protocol() const override { return "sftp"; }
    RemoteCapabilities capabilities() const override { return {}; }
    bool lost() const override { return false; }
    std::string home() override { return "/"; }

    std::string realpath(const std::string& path) override {
        std::lock_guard l(mu);
        const auto r = resolve(path, 0);
        if (!r) throw SftpError("No such file: " + path);
        return *r;
    }

    std::vector<RemoteEntry> list(const std::string& path, bool, const std::atomic<bool>*) override {
        std::lock_guard l(mu);
        const auto real = resolve(path, 0);
        if (!real) throw SftpError("No such folder: " + path);
        std::vector<RemoteEntry> out;
        if (*real == "/endless" || real->rfind("/endless/", 0) == 0) {
            RemoteEntry e;
            e.entry = entry(path, "d", "dir", 0, 0755);
            out.push_back(e);
            return out;
        }
        const std::string prefix = *real == "/" ? "/" : *real + "/";
        for (const auto& [p, n] : nodes) {
            if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string name = p.substr(prefix.size());
            if (name.find('/') != std::string::npos) continue;
            RemoteEntry e;
            e.entry = entry(path, name, n.type, n.content.size(), n.mode);
            if (n.type == "link") {
                const auto t = resolve(n.target, 0);
                const Node* tn = t ? find(*t) : nullptr;
                e.entry.linkTargetIsDir = tn && tn->type == "dir";
                e.targetSize = tn && tn->type == "file" ? tn->content.size() : 0;
            }
            out.push_back(e);
        }
        return out;
    }

    std::optional<SftpEntry> stat(const std::string& path, bool follow) override {
        std::lock_guard l(mu);
        if (path == "/endless" || path.rfind("/endless/", 0) == 0) return entry("/", "endless", "dir", 0, 0755);
        const auto real = follow ? resolve(path, 0) : std::optional<std::string>(path);
        const Node* n = real ? find(*real) : nullptr;
        if (!n) return std::nullopt;
        SftpEntry e = entry("/", sftputil::remoteBaseName(path), n->type, n->content.size(), n->mode);
        e.path = path;
        return e;
    }

    void mkdir(const std::string& path, uint32_t) override {
        std::lock_guard l(mu);
        nodes[path] = Node{"dir", "", 0755, ""};
    }
    void rename(const std::string&, const std::string&) override {}
    void unlink(const std::string& path) override {
        std::lock_guard l(mu);
        nodes.erase(path);
    }
    void rmdir(const std::string& path) override {
        std::lock_guard l(mu);
        nodes.erase(path);
    }
    void chmod(const std::string&, uint32_t) override {}
    void setMtime(const std::string&, int64_t) override {}

    std::unique_ptr<RemoteReader> openRead(const std::string& path, uint64_t offset) override {
        std::lock_guard l(mu);
        const auto real = resolve(path, 0);
        const Node* n = real ? find(*real) : nullptr;
        if (!n || n->type != "file") throw SftpError("Cannot open " + quotePath(path));
        return std::make_unique<Reader>(n->content.substr(static_cast<size_t>(offset)));
    }

    std::unique_ptr<RemoteWriter> openWrite(const std::string& path, const WriteSpec& spec) override {
        std::lock_guard l(mu);
        if (!nodes.count(path)) createdModes[path] = spec.mode;
        return std::make_unique<Writer>(*this, path, spec.mode);
    }
    void close() override {}

private:
    struct Reader final : RemoteReader {
        std::string data;
        bool sent = false;
        explicit Reader(std::string d) : data(std::move(d)) {}
        uint64_t size() const override { return data.size(); }
        int64_t mtime() const override { return 0; }
        std::shared_ptr<std::string> read(bool& done) override {
            done = true;
            auto out = std::make_shared<std::string>(sent ? std::string() : data);
            sent = true;
            return out;
        }
        void close() override {}
        void abort() noexcept override {}
    };
    struct Writer final : RemoteWriter {
        FakeFs& fs;
        std::string path, data;
        uint32_t mode;
        Writer(FakeFs& f, std::string p, uint32_t m) : fs(f), path(std::move(p)), mode(m) {}
        void write(const std::shared_ptr<std::string>& d, const ProgressFn& progress) override {
            data += *d;
            if (progress) progress(d->size());
        }
        void finish(int64_t, const ProgressFn&) override {
            std::lock_guard l(fs.mu);
            fs.nodes[path] = Node{"file", data, mode, ""};
        }
        void abort() noexcept override {}
    };

    static SftpEntry entry(const std::string& parent, const std::string& name, const std::string& type, size_t size,
                           uint32_t mode) {
        SftpEntry e;
        e.name = name;
        e.path = sftputil::joinRemote(parent, name);
        e.type = type;
        e.size = static_cast<double>(size);
        e.mode = mode;
        return e;
    }
    const Node* find(const std::string& p) const {
        auto it = nodes.find(p);
        return it == nodes.end() ? nullptr : &it->second;
    }
    /** The real path of `path`, following links in every component; nullopt when it leads nowhere. */
    std::optional<std::string> resolve(const std::string& path, int hops) const {
        if (hops > 40) return std::nullopt;
        std::string real = "/";
        size_t start = 1;
        while (start <= path.size()) {
            size_t slash = path.find('/', start);
            if (slash == std::string::npos) slash = path.size();
            const std::string part = path.substr(start, slash - start);
            start = slash + 1;
            if (part.empty()) continue;
            std::string next = real == "/" ? "/" + part : real + "/" + part;
            if (real == "/endless" || real.rfind("/endless/", 0) == 0 || next == "/endless") {
                real = next;
                continue;
            }
            const Node* n = find(next);
            if (!n) return std::nullopt;
            if (n->type == "link") {
                const auto t = resolve(n->target, hops + 1);
                if (!t) return std::nullopt;
                next = *t;
            }
            real = next;
        }
        return real;
    }
};

/** The transfer engine on a FakeFs; waits for transfers to end. */
struct Engine {
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, TransferProgress> last;
    std::shared_ptr<FakeFs> fs = std::make_shared<FakeFs>();
    TransferEngine engine{EngineHooks{[this](const TransferProgress& p) {
                                          std::lock_guard l(m);
                                          last[p.transferId] = p;
                                          cv.notify_all();
                                      },
                                      nullptr}};
    Engine() { engine.addQueue("s", fs, 1); }
    ~Engine() { engine.stop(); }

    TransferProgress run(TransferKind kind, const std::string& remote, const fs::path& local) {
        TransferOptions o;
        o.conflict = "overwrite";
        auto t = engine.create("s", kind, remote, local, "x", o, nullptr);
        engine.enqueue(t);
        std::unique_lock l(m);
        REQUIRE(cv.wait_for(l, 30s, [&] {
            auto it = last.find(t->id);
            return it != last.end() && (it->second.state == "done" || it->second.state == "error" ||
                                        it->second.state == "cancelled");
        }));
        return last[t->id];
    }
};

/** A link to the folder `target` at `link`: a symbolic link, or where that needs a privilege (Windows without
 *  Developer Mode), a junction, which Windows lets anyone make and which the walk treats the same. */
bool folderLink(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (!ec) return true;
    if (std::string(sys::osName()) != "win32") return false;
    const auto r = runCommand("mklink /J \"" + sys::fromPath(link) + "\" \"" + sys::fromPath(target) + "\"");
    return r.code == 0 && fs::exists(link);
}

std::string slurpFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** A folder `levels` deep under `root` ("root/d/d/..."), with a file at the bottom; returns the bottom folder. */
std::string deepTree(FakeFs& fs, const std::string& root, int levels) {
    fs.dir(root);
    std::string p = root;
    for (int i = 0; i < levels; ++i) {
        p += "/d";
        fs.dir(p);
    }
    fs.file(p + "/bottom.txt", "deep");
    return p;
}

}  // namespace

TEST_CASE("tree limits: endless nesting stops every walk with a message; a deep real tree still works",
          "[sftp][tree][limits]") {
    Engine e;
    TempDir local;
    const std::string nested = "The folder is nested too deeply (more than 64 levels)";

    SECTION("download") {
        const auto p = e.run(TransferKind::Download, "/endless", local.path() / "endless");
        CHECK(p.state == "error");
        CHECK(p.error == nested);
        CHECK_FALSE(fs::exists(local.path() / "endless"));  // listed first: nothing was copied

        deepTree(*e.fs, "/deep", 50);
        const auto ok = e.run(TransferKind::Download, "/deep", local.path() / "deep");
        CHECK(ok.state == "done");
        fs::path bottom = local.path() / "deep";
        for (int i = 0; i < 50; ++i) bottom /= "d";
        CHECK(slurpFile(bottom / "bottom.txt") == "deep");
    }
    SECTION("delete (the default removeTree)") {
        CHECK_THROWS_WITH(e.fs->removeTree("/endless"), nested);
        deepTree(*e.fs, "/deep", 50);
        e.fs->removeTree("/deep");
        CHECK(e.fs->nodes.size() == 1);  // only "/"
    }
    SECTION("find: what was found so far, and the message") {
        FindRequest req;
        req.root = "/endless";
        req.pattern = "d";
        std::vector<FindEvent> events;
        std::atomic<bool> cancel{false};
        runFind(*e.fs, req, cancel, [&](FindEvent& ev) { events.push_back(ev); });
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().done);
        CHECK(events.back().error == nested);
        size_t found = 0;
        for (const auto& ev : events) found += ev.results.size();
        CHECK(found == 64);
    }
    SECTION("synchronize: refused before comparing") {
        SyncRequest req;
        const SyncFilter filter(req);
        std::atomic<bool> cancel{false};
        CHECK_THROWS_WITH(walkRemote(*e.fs, "/endless", filter, cancel), nested);
        deepTree(*e.fs, "/deep", 50);
        CHECK(walkRemote(*e.fs, "/deep", filter, cancel).size() == 51);
    }
    SECTION("entries: endless files in one folder") {
        setTreeLimitsForTests({64, 50});
        e.fs->dir("/wide");
        for (int i = 0; i < 60; ++i) e.fs->file("/wide/f" + std::to_string(i), "x");
        const auto p = e.run(TransferKind::Download, "/wide", local.path() / "wide");
        setTreeLimitsForTests({});
        CHECK(p.state == "error");
        CHECK(p.error == "The folder has more than 50 files and folders");
        CHECK(treeLimits().entries == 1000000);
    }
}

TEST_CASE("tree walks: links to folders are followed unless they lead back; links to files copy the file",
          "[sftp][tree][links]") {
    Engine e;
    TempDir local;
    FakeFs& f = *e.fs;
    f.dir("/outside");
    f.file("/outside/shared.txt", "shared");
    f.dir("/tree");
    f.dir("/tree/sub");
    f.file("/tree/sub/a.txt", "a");
    f.link("/tree/to-outside", "/outside");      // followed: copied as a folder
    f.link("/tree/to-sub", "/tree/sub");          // into the tree: copied anyway, skipped
    f.link("/tree/sub/to-parent", "/tree");       // a loop: skipped
    f.link("/outside/back", "/outside");          // a loop inside the followed folder: skipped
    f.link("/tree/to-file", "/tree/sub/a.txt");   // a link to a file: its content

    const auto p = e.run(TransferKind::Download, "/tree", local.path() / "tree");
    const fs::path l = local.path() / "tree";
    CHECK(slurpFile(l / "sub" / "a.txt") == "a");
    CHECK(slurpFile(l / "to-outside" / "shared.txt") == "shared");
    CHECK(slurpFile(l / "to-file") == "a");
    CHECK_FALSE(fs::exists(l / "to-sub"));
    CHECK_FALSE(fs::exists(l / "sub" / "to-parent"));
    CHECK_FALSE(fs::exists(l / "to-outside" / "back"));
    CHECK(p.skippedLinks == 3);
    CHECK(p.state == "done");

    // A dangling link is tried as a file, and its error names it.
    f.link("/tree/dangling", "/nowhere");
    const auto bad = e.run(TransferKind::Download, "/tree", local.path() / "tree2");
    CHECK(bad.state == "error");
    CHECK(bad.error.find("/tree/dangling") != std::string::npos);
}

TEST_CASE("tree walks: the executable bit", "[sftp][tree][mode]") {
    Engine e;
    TempDir local;
    e.fs->dir("/bin");
    e.fs->file("/bin/run.sh", "#!/bin/sh\n", 0100755);
    e.fs->file("/bin/suid", "x", 0104755);
    e.fs->file("/bin/plain.txt", "x", 0100644);
    REQUIRE(e.run(TransferKind::Download, "/bin", local.path() / "bin").state == "done");
    const auto bits = [&](const char* name) { return sys::permissionBits(local.path() / "bin" / name); };
    if (bits("run.sh")) {  // macOS: the executable bits arrive, setuid never
        CHECK((*bits("run.sh") & 0111) == 0111);
        CHECK((*bits("suid") & 07000) == 0);
        CHECK((*bits("suid") & 0111) == 0111);
        CHECK((*bits("plain.txt") & 0111) == 0);
    }

    // Upload: a new remote file gets the local permission bits; Windows has none, so 0644.
    spit(local.path() / "up.sh", "#!/bin/sh\n");
    std::error_code ec;
    fs::permissions(local.path() / "up.sh", fs::perms(0755), ec);
    e.fs->dir("/up");
    REQUIRE(e.run(TransferKind::Upload, "/up/up.sh", local.path() / "up.sh").state == "done");
    CHECK(e.fs->createdModes["/up/up.sh"] == (sys::permissionBits(local.path() / "up.sh") ? 0755u : 0644u));
}

TEST_CASE("tree walks: local links to folders are followed on upload unless they lead back", "[sftp][tree][links]") {
    Engine e;
    TempDir local, outside;
    const fs::path t = local.path() / "tree";
    fs::create_directories(t / "sub");
    spit(t / "sub" / "a.txt", "a");
    spit(outside.path() / "shared.txt", "shared");
    REQUIRE(folderLink(outside.path(), t / "to-outside"));
    REQUIRE(folderLink(t, t / "sub" / "to-parent"));
    e.fs->dir("/dest");
    const auto p = e.run(TransferKind::Upload, "/dest/tree", t);
    CHECK(p.state == "done");
    CHECK(e.fs->nodes.count("/dest/tree/sub/a.txt") == 1);
    CHECK(e.fs->nodes.count("/dest/tree/to-outside/shared.txt") == 1);
    CHECK(e.fs->nodes.count("/dest/tree/sub/to-parent") == 0);  // before: an empty folder on the server
    CHECK(p.skippedLinks == 1);
}
