#pragma once
// Pseudo-terminals for local shells: a program started on a new pseudo-terminal, its output read, its input written,
// its window size changed, its exit awaited and it ended with everything it started (Windows: ConPTY, Windows 10 1809
// and later, a job object for the process tree; POSIX: openpty, a new session, SIGHUP to its process group). Plus the
// shells installed on this computer.
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::sys::pty {

struct Options {
    /** The program and its arguments as one command line. Windows: as CreateProcess takes it
     *  (`"C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo`). POSIX: split as a shell would (spaces, '...', "...",
     *  backslash), the program looked up in PATH when it has no '/'. */
    std::string command;
    /** The folder the program starts in (UTF-8); must exist. */
    std::string startDir;
    /** Set on top of this process' environment; an empty value removes the variable. */
    std::vector<std::pair<std::string, std::string>> environment;
    int cols = 80;
    int rows = 24;
};

/** True when this computer can host pseudo-terminals; otherwise `reason` says why (Windows older than 10 1809). */
bool supported(std::string& reason);

struct ExitStatus {
    int code = 0;        // the exit code (POSIX: 128 + the signal number when a signal ended it)
    std::string signal;  // POSIX: "HUP", "KILL", ... when a signal ended it; empty otherwise
};

/**
 * One program on one pseudo-terminal. Threads: read() from one thread, write() from one thread, wait() from one
 * thread; resize(), finishOutput() and kill() from any. Destroying it ends the program if it still runs.
 */
class Pty {
public:
    Pty();
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    /** Starts the program; false with a readable `error` ("Cannot start ...: The system cannot find the file
     *  specified."). Only once per object. */
    bool start(const Options& options, std::string& error);
    /** Blocks until output arrives: the number of bytes read into `buf` (> 0), or 0 when the output has ended
     *  (after finishOutput(), once the output still buffered has been read). */
    std::size_t read(char* buf, std::size_t size);
    /** Writes all of `bytes`, blocking while the program does not read; false when the terminal is gone. */
    bool write(std::string_view bytes);
    void resize(int cols, int rows);
    /** Blocks until the program exits. */
    ExitStatus wait();
    /** After wait(): read() returns what is still buffered, then 0 (Windows: closes the pseudo console, which only
     *  then ends its output). */
    void finishOutput();
    /** Ends the program and every process it started (Windows: the job object; POSIX: SIGHUP to its process group,
     *  SIGKILL after 2 seconds). Any time after start(). */
    void kill();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct Shell {
    std::string id;       // stable: "pwsh", "cmd", "git-bash", "wsl:Ubuntu", "/bin/zsh", ...
    std::string name;     // "PowerShell 7", "Git Bash", "Ubuntu (WSL)", "zsh", ...
    std::string command;  // as Options::command
    std::vector<std::pair<std::string, std::string>> environment;  // what it needs on top (MSYS2: MSYSTEM, ...)
    bool isDefault = false;  // the one a new local terminal opens by default (exactly one when the list is not empty)
};

/** The shells installed here, in a sensible menu order. Windows: Command Prompt, Windows PowerShell, PowerShell 7,
 *  Git Bash, Cygwin, MSYS2 and the WSL distributions; macOS: the login shell and the others in /etc/shells. */
std::vector<Shell> listShells();

}  // namespace st::sys::pty
