// Local terminal transport: a program in a pseudo-terminal (platform/sys/Pty.h). Three threads: run() starts the
// program and waits for it to exit, a reader passes its output on, a writer feeds it the queued input (writing can
// block while the program does not read, and Transport::write must not).
#include "transport/LocalTransport.h"

#include "core/Strings.h"
#include "platform/sys/Debug.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Pty.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace st {

namespace {

std::optional<sys::pty::Shell> defaultShell() {
    std::vector<sys::pty::Shell> shells = sys::pty::listShells();
    for (auto& s : shells)
        if (s.isDefault) return s;
    if (!shells.empty()) return shells.front();
    return std::nullopt;
}

/** The start folder: "" = home, "~" or "~/..." relative to home. */
std::string startFolder(const std::string& configured) {
    const std::string home = sys::fromPath(sys::homeDirectory());
    std::string d = trim(configured);
    if (d.empty()) return home;
    if (d == "~") return home;
    if (d.size() > 1 && d[0] == '~' && (d[1] == '/' || d[1] == '\\')) return home + d.substr(1);
    return d;
}

class LocalTransportImpl final : public Transport {
public:
    explicit LocalTransportImpl(TransportContext ctx)
        : ctx_(std::move(ctx)), cols_(ctx_.cols), rows_(ctx_.rows) {}
    ~LocalTransportImpl() override {
        close();
        if (thread_.joinable()) thread_.join();
    }

    Protocol protocol() const override { return Protocol::Local; }
    void start() override { thread_ = std::thread([this] { run(); }); }

    void write(std::string bytes) override {
        {
            std::lock_guard lock(mutex_);
            if (inputClosed_) return;
            input_ += bytes;
        }
        cv_.notify_all();
    }

    void resize(int cols, int rows, int, int) override {
        std::lock_guard lock(mutex_);
        cols_ = cols;
        rows_ = rows;
        if (started_) pty_.resize(cols, rows);
    }

    std::vector<SpecialCommandDef> specials() const override { return {}; }
    void sendSpecial(const std::string&) override {}

    void close() override {
        std::lock_guard lock(mutex_);
        closing_ = true;
        if (started_) pty_.kill();
    }

    LdiscModes defaultLdisc() const override { return {false, false}; }

private:
    void log(LogLevel l, const std::string& m) {
        if (ctx_.listener) ctx_.listener->onLog(l, m);
    }

    void fail(const std::string& reason) {
        log(LogLevel::Error, reason);
        CloseInfo info;
        info.reason = reason;
        info.error = true;
        if (ctx_.listener) ctx_.listener->onClose(info);
    }

    sys::pty::Options options(std::string& error) {
        sys::pty::Options o;
        const SessionConfig& s = ctx_.session;
        o.command = trim(s.local.command);
        if (o.command.empty()) {
            const auto shell = defaultShell();
            if (!shell) {
                error = "No shell was found on this computer";
                return o;
            }
            o.command = shell->command;
            o.environment = shell->environment;
        }
        o.startDir = startFolder(s.local.startDir);
        o.cols = ctx_.cols;
        o.rows = ctx_.rows;
        // What terminal programs look at to know what they run in; then the session's own variables, which win.
        const std::string term = trim(s.connection.terminalType);
        o.environment.emplace_back("TERM", term.empty() ? "xterm-256color" : term);
        o.environment.emplace_back("COLORTERM", "truecolor");
        o.environment.emplace_back("TERM_PROGRAM", "SSHTermaq");
        if (!sys::getEnv("LANG") && !sys::getEnv("LC_ALL") && !sys::getEnv("LC_CTYPE")) {
            const std::string locale = sys::terminalLocale();
            if (!locale.empty()) o.environment.emplace_back("LANG", locale);
        }
        for (const EnvVar& v : s.connection.environment)
            if (!trim(v.name).empty()) o.environment.emplace_back(trim(v.name), v.value);
        return o;
    }

    void run() {
        sys::setThreadName("local");
        std::string error;
        sys::pty::Options o = options(error);
        if (!error.empty()) {
            fail(error);
            return;
        }
        const std::string name = localProgramName(o.command);
        if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connecting, "Starting " + name);
        log(LogLevel::Info, "Starting " + o.command + " in " + o.startDir);
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                fail("Cancelled");
                return;
            }
            o.cols = cols_;
            o.rows = rows_;
            if (!pty_.start(o, error)) {
                fail(error);
                return;
            }
            started_ = true;
        }
        if (ctx_.listener) {
            ctx_.listener->onServerInfo(name);
            ctx_.listener->onStatus(ConnStatus::Connected, "");
        }

        std::thread reader([this] {
            sys::setThreadName("local-read");
            std::vector<char> buf(64 * 1024);
            while (const std::size_t n = pty_.read(buf.data(), buf.size()))
                if (ctx_.listener) ctx_.listener->onData(std::string_view(buf.data(), n));
        });
        std::thread writer([this] {
            sys::setThreadName("local-write");
            for (;;) {
                std::string chunk;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return !input_.empty() || inputClosed_; });
                    if (input_.empty()) return;
                    chunk.swap(input_);
                }
                if (!pty_.write(chunk)) return;
            }
        });

        const sys::pty::ExitStatus st = pty_.wait();
        pty_.finishOutput();  // the rest of its output, then the end
        reader.join();
        {
            std::lock_guard lock(mutex_);
            inputClosed_ = true;
        }
        cv_.notify_all();
        writer.join();

        bool byUser = false;
        {
            std::lock_guard lock(mutex_);
            byUser = closing_;
        }
        CloseInfo info;
        info.hasExitCode = true;
        info.exitCode = st.code;
        info.exitSignal = st.signal;
        info.clean = byUser || (st.code == 0 && st.signal.empty());
        info.reason = byUser                ? "Closed"
                      : !st.signal.empty() ? name + " was ended by signal " + st.signal
                                           : name + " exited with code " + std::to_string(st.code);
        log(info.clean ? LogLevel::Info : LogLevel::Warn, info.reason);
        if (ctx_.listener) ctx_.listener->onClose(info);
    }

    TransportContext ctx_;
    std::thread thread_;
    sys::pty::Pty pty_;
    std::mutex mutex_;  // everything below
    std::condition_variable cv_;
    std::string input_;
    bool inputClosed_ = false;
    bool started_ = false;
    bool closing_ = false;
    int cols_, rows_;
};

}  // namespace

std::unique_ptr<Transport> makeLocalTransport(TransportContext ctx) { return std::make_unique<LocalTransportImpl>(std::move(ctx)); }

std::vector<LocalShellInfo> listLocalShells() {
    std::vector<LocalShellInfo> out;
    std::string reason;
    if (!sys::pty::supported(reason)) return out;
    for (const auto& s : sys::pty::listShells()) {
        LocalShellInfo info{s.id, s.name, s.command, {}, s.isDefault};
        for (const auto& [k, v] : s.environment) info.environment.push_back({k, v});
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace st
