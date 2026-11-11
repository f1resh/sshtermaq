#pragma once
// Application diagnostics log (not the per-connection event log / session log).
// Writes to the debug output (sys::debugOutput) and <dataDir>/logs/sshtermaq.log once initialized.
#include <cstdint>
#include <filesystem>
#include <string>

namespace st::applog {

/** Size past which init() moves the log to previousLogFile() and starts a new one. */
inline constexpr std::uintmax_t kRotateBytes = 2 * 1024 * 1024;

/** Opens `file` for appending (rotating it first when it is over kRotateBytes); before this, lines go only to the debug
 *  output. Calling it again switches to another file. */
void init(const std::filesystem::path& file);
/** Where init() keeps the previous log: sshtermaq.log -> sshtermaq.1.log. */
std::filesystem::path previousLogFile(const std::filesystem::path& file);
/** Stops writing to the file (lines go to the debug output only, as before init()). */
void close();
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);
void debug(const std::string& msg);

}  // namespace st::applog
