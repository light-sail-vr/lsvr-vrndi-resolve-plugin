// NDI_LOG sink for Linux (docs/linux-port-spec.md decision 16).
//
// Live capture: syslog(3) with ident "NDIOutput", so over SSH
//     journalctl -f -t NDIOutput
// follows the plugin inside a desktop-launched Resolve (whose stdout goes
// nowhere — the old printf fallback was invisible). Optional file sink,
// mirroring Windows: set NDI_OUTPUT_LOG_FILE to a writable path before
// launching Resolve and every line is appended there too.
//
// Thread-safe: glibc's syslog() serialises internally and stdio locks each
// FILE*; render threads log concurrently. Header-only so the plugin and the
// CUDA module (CudaGPUAcceleration.cu) share one sink and one FILE*.
//
// openlog() sets the ident process-wide, i.e. for Resolve too. Called once,
// lazily, on the first plugin log line; LOG_PID keeps lines attributable.
// (Host-side syslog use by Resolve is unverified — ticket #73 checks.)

#ifndef LINUX_LOG_H
#define LINUX_LOG_H

#include <syslog.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "PlatformPaths.h"

namespace ndi_linux_log {

// Appends one already-formatted line (no trailing newline) to both sinks.
inline void writeLine(const char* line, size_t len)
{
    static std::once_flag opened;
    std::call_once(opened, [] { openlog("NDIOutput", LOG_PID | LOG_NDELAY, LOG_USER); });
    syslog(LOG_INFO, "%.*s", static_cast<int>(len), line);

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
