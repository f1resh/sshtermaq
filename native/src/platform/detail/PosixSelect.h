#pragma once
// select(2) for descriptors of any number (POSIX implementation detail of the pty and serial code). macOS' poll(2)
// does not work on devices (ptys, ttys), so those wait with select(2); its fd_set is fixed at FD_SETSIZE (1024)
// descriptors, and FD_SET with a higher number writes past it. NetInit raises the descriptor limit beyond that, so
// the sets here are sized to their highest descriptor instead. On macOS a file including this header must define
// _DARWIN_UNLIMITED_SELECT before any system header, so that select(2) accepts sets larger than fd_set.
#if defined(__APPLE__) && !defined(_DARWIN_UNLIMITED_SELECT)
#error "define _DARWIN_UNLIMITED_SELECT before any system header (see PosixSelect.h)"
#endif

#include <sys/select.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace st::sys::detail {

/** A descriptor set for select(2) as large as its highest descriptor needs. */
class FdSet {
public:
    void add(int fd) {
        const size_t word = static_cast<size_t>(fd) / NFDBITS;
        if (bits_.size() <= word) bits_.resize(std::max(word + 1, sizeof(fd_set) / sizeof(fd_mask)), 0);
        bits_[word] |= bit(fd);
        maxFd_ = std::max(maxFd_, fd);
    }
    bool has(int fd) const {
        const size_t word = static_cast<size_t>(fd) / NFDBITS;
        return word < bits_.size() && (bits_[word] & bit(fd));
    }
    fd_set* get() { return bits_.empty() ? nullptr : reinterpret_cast<fd_set*>(bits_.data()); }
    int count() const { return maxFd_ + 1; }

private:
    static fd_mask bit(int fd) {
        return static_cast<fd_mask>(std::make_unsigned_t<fd_mask>{1} << (static_cast<unsigned>(fd) % NFDBITS));
    }

    std::vector<fd_mask> bits_;
    int maxFd_ = -1;
};

/** Waits for `readA` / `readB` to be readable or `writeFd` writable (-1 = not watched); timeoutMs < 0 = no limit.
 *  The ready ones are set; false on error (errno). EINTR is retried. */
inline bool waitFds(int readA, int readB, int writeFd, int timeoutMs, bool& aReady, bool& bReady, bool& writable) {
    FdSet reads, writes;
    for (int fd : {readA, readB})
        if (fd >= 0) reads.add(fd);
    if (writeFd >= 0) writes.add(writeFd);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int n;
    do {
        n = ::select(std::max(reads.count(), writes.count()), reads.get(), writes.get(), nullptr,
                     timeoutMs < 0 ? nullptr : &tv);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return false;
    aReady = readA >= 0 && reads.has(readA);
    bReady = readB >= 0 && reads.has(readB);
    writable = writeFd >= 0 && writes.has(writeFd);
    return true;
}

}  // namespace st::sys::detail
