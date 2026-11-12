// Pseudo-terminals on POSIX (platform/sys/Pty.h): openpty, then fork. The child starts a new session with the terminal
// as its controlling terminal, restores the signal handling we change (we ignore SIGPIPE: a shell must not inherit
// that), closes every other descriptor of ours and execs; everything it needs is prepared before fork. The master is
// non-blocking and waited on with select(2) (poll(2) does not work on devices on macOS), together with a pipe that
// finishOutput() writes to. A finished program's terminal can stay open (a background job still has it), so the
// output ends when finishOutput() says so, not only when the terminal reports the end. openpty comes from <util.h>
// (macOS / BSD).
#include "platform/sys/Pty.h"

#include "platform/sys/Errors.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <pwd.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <util.h>

extern char** environ;

namespace st::sys::pty {

namespace {

/** Splits a command line as a shell would for a simple command: blanks separate words; '...' is literal; "..." keeps
 *  blanks and takes \" \\ \$ \` as escapes; a backslash outside quotes escapes the next character. */
std::vector<std::string> splitCommand(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool inWord = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            if (inWord) {
                out.push_back(std::move(cur));
                cur.clear();
                inWord = false;
            }
            continue;
        }
        inWord = true;
        if (c == '\'') {
            const size_t end = s.find('\'', i + 1);
            cur.append(s.substr(i + 1, (end == std::string_view::npos ? s.size() : end) - i - 1));
            i = end == std::string_view::npos ? s.size() : end;
        } else if (c == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size() && std::strchr("\"\\$`", s[i + 1])) ++i;
                cur += s[i];
            }
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i];
        } else {
            cur += c;
        }
    }
    if (inWord) out.push_back(std::move(cur));
    return out;
}

bool isExecutableFile(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

/** `name` itself when it has a '/', else the first executable `dir/name` of `path` (':'-separated); "" if none. */
std::string findProgram(const std::string& name, const std::string& path) {
    if (name.find('/') != std::string::npos) return isExecutableFile(name) ? name : std::string();
    size_t start = 0;
    for (;;) {
        const size_t end = path.find(':', start);
        std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (dir.empty()) dir = ".";
        const std::string candidate = dir + "/" + name;
        if (isExecutableFile(candidate)) return candidate;
        if (end == std::string::npos) return {};
        start = end + 1;
    }
}

std::string nameOf(const std::string& entry) { return entry.substr(0, entry.find('=')); }

/** This process' environment with `overrides` applied (an empty value removes the variable). */
std::vector<std::string> environmentWith(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::set<std::string> replaced;
    for (const auto& kv : overrides) replaced.insert(kv.first);
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e)
        if (!replaced.count(nameOf(*e))) env.emplace_back(*e);
    for (const auto& [k, v] : overrides)
        if (!k.empty() && !v.empty()) env.push_back(k + "=" + v);
    return env;
}

std::string signalName(int sig) {
    switch (sig) {
        case SIGHUP: return "HUP";
        case SIGINT: return "INT";
        case SIGQUIT: return "QUIT";
        case SIGILL: return "ILL";
        case SIGABRT: return "ABRT";
        case SIGFPE: return "FPE";
        case SIGKILL: return "KILL";
        case SIGSEGV: return "SEGV";
        case SIGPIPE: return "PIPE";
        case SIGALRM: return "ALRM";
        case SIGTERM: return "TERM";
        case SIGBUS: return "BUS";
        default: return std::to_string(sig);
    }
}

void setFlags(int fd, int fdFlags, int statusFlags) {
    if (fdFlags) ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | fdFlags);
    if (statusFlags) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | statusFlags);
}

}  // namespace

bool supported(std::string&) { return true; }

struct Pty::Impl {
    int master = -1;
    int wake[2] = {-1, -1};  // finishOutput() writes to wake[1]
    pid_t pid = -1;
    std::shared_ptr<std::atomic<bool>> reaped = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> finishing{false};
    std::mutex killMutex;
    bool killScheduled = false;

    /** SIGHUP (and SIGCONT, in case it is stopped) to the program's process group; SIGKILL 2 seconds later if it has
     *  not exited by then. */
    void hangUp() {
        std::lock_guard lock(killMutex);
        if (pid <= 0 || *reaped) return;
        ::killpg(pid, SIGHUP);
        ::killpg(pid, SIGCONT);
        if (killScheduled) return;
        killScheduled = true;
        std::thread([p = pid, done = reaped] {
            for (int i = 0; i < 20 && !*done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!*done) ::killpg(p, SIGKILL);
        }).detach();
    }

    ~Impl() {
        if (pid > 0 && !*reaped) {
            hangUp();
            // Nobody waits for it any more: reap it in the background so it does not stay a zombie.
            std::thread([p = pid, done = reaped] {
                int status = 0;
                while (::waitpid(p, &status, 0) < 0 && errno == EINTR) {
                }
                *done = true;
            }).detach();
        }
        for (int fd : {master, wake[0], wake[1]})
            if (fd >= 0) ::close(fd);
    }
};

Pty::Pty() : impl_(std::make_unique<Impl>()) {}
Pty::~Pty() = default;

bool Pty::start(const Options& o, std::string& error) {
    const std::vector<std::string> args = splitCommand(o.command);
    if (args.empty()) {
        error = "No program to start";
        return false;
    }
    const std::vector<std::string> env = environmentWith(o.environment);
    std::string path = "/usr/bin:/bin:/usr/sbin:/sbin";
    for (const auto& e : env)
        if (nameOf(e) == "PATH") path = e.substr(5);
    const std::string program = findProgram(args[0], path);
    if (program.empty()) {
        error = "Cannot start \"" + o.command + "\": " + args[0] + " was not found";
        return false;
    }
    struct stat st {};
    if (o.startDir.empty() || ::stat(o.startDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        error = "The start folder \"" + o.startDir + "\" does not exist";
        return false;
    }

    // Everything the child uses, ready before fork: after it, the child may only make async-signal-safe calls.
    std::vector<char*> argv, envp;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (const auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    const int maxFd = static_cast<int>(std::min<long>(::sysconf(_SC_OPEN_MAX), 65536));
    static const char kExecFailed[] = "\r\nCannot start the program: exec failed\r\n";

    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(std::clamp(o.cols, 1, 65535));
    ws.ws_row = static_cast<unsigned short>(std::clamp(o.rows, 1, 65535));
    int slave = -1;
    if (::openpty(&impl_->master, &slave, nullptr, nullptr, &ws) != 0) {
        error = "Cannot open a pseudo-terminal: " + lastErrorMessage();
        return false;
    }
    setFlags(impl_->master, FD_CLOEXEC, O_NONBLOCK);
    setFlags(slave, FD_CLOEXEC, 0);
    if (::pipe(impl_->wake) != 0) {
        error = "Cannot open a pseudo-terminal: " + lastErrorMessage();
        ::close(slave);
        return false;
    }
    for (int fd : impl_->wake) setFlags(fd, FD_CLOEXEC, O_NONBLOCK);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::ioctl(slave, TIOCSCTTY, 0);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        ::dup2(slave, STDERR_FILENO);
        for (int fd = 3; fd < maxFd; ++fd) ::close(fd);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        for (int sig = 1; sig < NSIG; ++sig) ::sigaction(sig, &dfl, nullptr);  // SIGKILL / SIGSTOP refuse: fine
        if (::chdir(o.startDir.c_str()) != 0) {
        }
        ::execve(program.c_str(), argv.data(), envp.data());
        ssize_t ignored = ::write(STDERR_FILENO, kExecFailed, sizeof(kExecFailed) - 1);
        (void)ignored;
        ::_exit(127);
    }
    ::close(slave);
    if (pid < 0) {
        error = "Cannot start \"" + o.command + "\": " + lastErrorMessage();
        return false;
    }
    impl_->pid = pid;
    return true;
}

std::size_t Pty::read(char* buf, std::size_t size) {
    const int m = impl_->master;
    if (m < 0) return 0;
    for (;;) {
        if (impl_->finishing) {
            // What is still buffered, then the end.
            const ssize_t n = ::read(m, buf, size);
            return n > 0 ? static_cast<std::size_t>(n) : 0;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(m, &rd);
        FD_SET(impl_->wake[0], &rd);
        const int rc = ::select(std::max(m, impl_->wake[0]) + 1, &rd, nullptr, nullptr, nullptr);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (FD_ISSET(impl_->wake[0], &rd)) {
            char drain[16];
            while (::read(impl_->wake[0], drain, sizeof drain) > 0) {
            }
            continue;  // finishing is set: the loop reads what is left
        }
        const ssize_t n = ::read(m, buf, size);
        if (n > 0) return static_cast<std::size_t>(n);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        return 0;  // 0 or EIO: every process closed the terminal
    }
}

bool Pty::write(std::string_view bytes) {
    const int m = impl_->master;
    while (!bytes.empty()) {
        if (m < 0 || impl_->finishing) return false;
        const ssize_t n = ::write(m, bytes.data(), bytes.size());
        if (n > 0) {
            bytes.remove_prefix(static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            // The program does not read: wait, but look at `finishing` now and then.
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(m, &wr);
            timeval tv{0, 200000};
            ::select(m + 1, nullptr, &wr, nullptr, &tv);
            continue;
        }
        return false;
    }
    return true;
}

void Pty::resize(int cols, int rows) {
    if (impl_->master < 0) return;
    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(std::clamp(cols, 1, 65535));
    ws.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
    ::ioctl(impl_->master, TIOCSWINSZ, &ws);  // the kernel sends SIGWINCH to the foreground process group
}

ExitStatus Pty::wait() {
    ExitStatus st;
    if (impl_->pid <= 0) return st;
    int status = 0;
    pid_t r;
    while ((r = ::waitpid(impl_->pid, &status, 0)) < 0 && errno == EINTR) {
    }
    *impl_->reaped = true;
    if (r < 0) return st;
    if (WIFEXITED(status)) {
        st.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        st.code = 128 + WTERMSIG(status);
        st.signal = signalName(WTERMSIG(status));
    }
    return st;
}

void Pty::finishOutput() {
    impl_->finishing = true;
    if (impl_->wake[1] >= 0) {
        ssize_t ignored = ::write(impl_->wake[1], "x", 1);
        (void)ignored;
    }
}

void Pty::kill() { impl_->hangUp(); }

// --------------------------------------------------------------------------------------------------- shells ---

namespace {

std::string loginShell() {
    std::vector<char> buf(16384);
    struct passwd pw {};
    struct passwd* found = nullptr;
    if (::getpwuid_r(::getuid(), &pw, buf.data(), buf.size(), &found) == 0 && found && found->pw_shell && *found->pw_shell)
        return found->pw_shell;
    if (const char* s = ::getenv("SHELL"); s && *s) return s;
    return "/bin/zsh";
}

std::string quoted(const std::string& path) {
    if (path.find_first_of(" \t'\"\\") == std::string::npos) return path;
    std::string q = "'";
    for (char c : path) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

}  // namespace

std::vector<Shell> listShells() {
    std::vector<std::string> paths{loginShell()};
    std::ifstream in("/etc/shells");
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || line[0] != '/') continue;
        paths.push_back(line);
    }
    std::vector<Shell> out;
    std::set<std::string> seenPaths, seenNames;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string& p = paths[i];
        if (!isExecutableFile(p) || !seenPaths.insert(p).second) continue;
        const std::string base = p.substr(p.rfind('/') + 1);
        std::string name = seenNames.insert(base).second ? base : base + " (" + p.substr(0, p.rfind('/')) + ")";
        // A login shell, as Terminal.app starts one: it reads the profile files, which set PATH for an app started
        // from the Finder.
        out.push_back({p, name, quoted(p) + " -l", {}, out.empty() && i == 0});
    }
    if (!out.empty() && std::none_of(out.begin(), out.end(), [](const Shell& s) { return s.isDefault; })) out[0].isDefault = true;
    return out;
}

}  // namespace st::sys::pty
