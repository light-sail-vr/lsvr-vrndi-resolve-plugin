// NDI_LOG sink for Linux (docs/linux-port-spec.md decision 16).
//
// Live capture: syslog (journald) under the tag "NDIOutput", so over SSH
//     journalctl -f -t NDIOutput
// follows the plugin inside a desktop-launched Resolve (whose stdout goes
// nowhere — the old printf fallback was invisible). Optional file sink,
// mirroring Windows: set NDI_OUTPUT_LOG_FILE to a writable path before
// launching Resolve and every line is appended there too.
//
// Per-message tag, never openlog(): glibc keeps ONE ident per process, and
// Resolve (Qt, glib, the resolve binary) logs through syslog() as
// "resolve". openlog("NDIOutput") from the plugin re-tagged every later
// Resolve syslog line as NDIOutput (verified 2026-10-30, ticket #73; see
// LEARNINGS). So each line goes to /dev/log as its own RFC 3164 datagram,
// tagged NDIOutput[pid], the format glibc's syslog() writes: journald
// derives SYSLOG_IDENTIFIER from the tag, and the process-wide syslog
// state stays Resolve's.
//
// Thread-safe: one mutex serialises the socket (render threads log
// concurrently) and stdio locks each FILE*. Header-only so the plugin and
// the CUDA module (CudaGPUAcceleration.cu) share one sink and one FILE*.
// Never blocks a render thread on journald: the socket is non-blocking (a
// full queue drops the line), and without /dev/log (a container) lines are
// dropped there silently, retrying the connect at most once per
// kReconnectBackoff rather than per line; the file sink still works.

#ifndef LINUX_LOG_H
#define LINUX_LOG_H

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include "PlatformPaths.h"

namespace ndi_linux_log {

// "<14>Mmm dd hh:mm:ss NDIOutput[pid]: msg" into buf (NUL-terminated,
// truncating the message to fit); returns the datagram length. 14 is
// LOG_USER|LOG_INFO. Month names are fixed English (RFC 3164), never the
// host's locale.
inline size_t formatDatagram(char* buf, size_t cap, const std::tm& when, long pid,
                             const char* msg, size_t len)
{
    static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (cap == 0) {
        return 0;
    }
    const int mon = (when.tm_mon >= 0 && when.tm_mon < 12) ? when.tm_mon : 0;
    const int head = std::snprintf(buf, cap, "<14>%s %2d %02d:%02d:%02d NDIOutput[%ld]: ",
                                   kMonths[mon], when.tm_mday, when.tm_hour, when.tm_min,
                                   when.tm_sec, pid);
    if (head < 0) {
        buf[0] = '\0';
        return 0;
    }
    size_t n = static_cast<size_t>(head);
    if (n >= cap) {
        return cap - 1; // snprintf truncated the header and terminated it
    }
    const size_t room = cap - 1 - n;
    const size_t take = len < room ? len : room;
    std::memcpy(buf + n, msg, take);
    n += take;
    buf[n] = '\0';
    return n;
}

// How long a failed /dev/log connect is remembered before trying again.
constexpr std::chrono::seconds kReconnectBackoff{5};

// Rate-limits connect attempts after a failure, so a host without /dev/log
// costs one socket()+connect() per kReconnectBackoff, not one per line.
// Not thread-safe; the caller holds the sink's mutex.
struct ReconnectGate {
    bool failed = false;
    std::chrono::steady_clock::time_point lastFailure{};

    bool allowed(std::chrono::steady_clock::time_point now) const
    {
        return !failed || now - lastFailure >= kReconnectBackoff;
    }
    void fail(std::chrono::steady_clock::time_point now)
    {
        failed = true;
        lastFailure = now;
    }
    void succeed() { failed = false; }
};

namespace detail {

// Connected non-blocking datagram socket to /dev/log, or -1. Caller holds
// the mutex.
inline int connectDevLog()
{
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, "/dev/log", sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline void sendToSyslog(const char* line, size_t len)
{
    static std::mutex mutex;
    static int fd = -1;
    static ReconnectGate gate;
    char datagram[2304];
    const std::time_t now = std::time(nullptr);
    std::tm when = {};
    localtime_r(&now, &when);
    const size_t n = formatDatagram(datagram, sizeof(datagram), when,
                                    static_cast<long>(getpid()), line, len);

    std::lock_guard<std::mutex> lock(mutex);
    // One reconnect: journald restarting leaves the old socket dead.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (fd < 0) {
            const auto tick = std::chrono::steady_clock::now();
            if (!gate.allowed(tick)) {
                return; // /dev/log failed recently; drop without a syscall
            }
            fd = connectDevLog();
            if (fd < 0) {
                gate.fail(tick);
                return;
            }
            gate.succeed();
        }
        if (send(fd, datagram, n, MSG_NOSIGNAL) >= 0) {
            return;
        }
        if (errno == EINTR || errno == EAGAIN || errno == ENOBUFS) {
            return; // journald is behind; drop the line rather than block a render
        }
        close(fd);
        fd = -1;
    }
}

} // namespace detail

// Appends one already-formatted line (no trailing newline) to both sinks.
inline void writeLine(const char* line, size_t len)
{
    detail::sendToSyslog(line, len);

    static std::FILE* sink = []() -> std::FILE* {
        const char* path = std::getenv("NDI_OUTPUT_LOG_FILE");
        return (path && *path) ? ndi_path::fopenUtf8(path, "ab") : nullptr;
    }();
    if (sink) {
        std::fwrite(line, 1, len, sink);
        std::fputc('\n', sink);
        std::fflush(sink);
    }
}

inline void vlog(const char* fmt, va_list args)
{
    char buf[2048];
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) {
        return;
    }
    size_t len = static_cast<size_t>(n);
    if (len > sizeof(buf) - 1) {
        len = sizeof(buf) - 1; // truncated; vsnprintf NUL-terminated at the end
    }
    // Callers written for os_log sometimes end in "\n"; one line per entry.
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        --len;
    }
    writeLine(buf, len);
}

inline void write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void write(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vlog(fmt, args);
    va_end(args);
}

} // namespace ndi_linux_log

#endif // LINUX_LOG_H
