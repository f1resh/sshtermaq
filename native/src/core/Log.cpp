#include "core/Log.h"

#include "platform/sys/Debug.h"
#include "platform/sys/Time.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace st::applog {

namespace {
std::mutex g_mutex;
std::ofstream g_file;

void write(const char* level, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    sys::localTime(t, tm);
    char ts[32];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    std::string line = std::string(ts) + " [" + level + "] " + msg + "\n";
    std::lock_guard lock(g_mutex);
    sys::debugOutput(line);
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
}
}  // namespace

std::filesystem::path previousLogFile(const std::filesystem::path& file) {
    std::filesystem::path previous = file;
    previous.replace_extension();
    previous += ".1";
    previous += file.extension();
    return previous;
}

void init(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    if (g_file.is_open()) g_file.close();
    std::filesystem::create_directories(file.parent_path(), ec);
    // Keep the log small: past 2 MB it becomes <name>.1.log (replacing the one before) and a new log starts, so the
    // lines before the problem a user reports are still there.
    if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > kRotateBytes) {
        // Fails while another instance has the file open (a second start forwarding its command line): then keep
        // appending, the next start rotates it.
        std::filesystem::rename(file, previousLogFile(file), ec);
    }
    g_file.open(file, std::ios::out | std::ios::app);
}

void close() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
}

void info(const std::string& msg) { write("INFO", msg); }
void warn(const std::string& msg) { write("WARN", msg); }
void error(const std::string& msg) { write("ERROR", msg); }
void debug(const std::string& msg) {
#ifndef NDEBUG
    write("DEBUG", msg);
#else
    (void)msg;
#endif
}

}  // namespace st::applog
