// Tests for the Linux log sink's syslog datagram (src/LinuxLog.h): the plugin
// tags each message itself instead of calling openlog(), which would re-tag
// every syslog line of Resolve's process (LEARNINGS 2026-10-30).
// Build & run: ctest (Linux); on macOS by hand:
//   c++ -std=c++17 -DSOCK_CLOEXEC=0 -Isrc tests/test_linux_log.cpp -o build/t && build/t

#include "LinuxLog.h"

#include <cstdio>
#include <ctime>
#include <string>

static int failures = 0;

static void expectTrue(bool actual, const char* name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

static std::string datagram(const char* msg, size_t len, size_t cap = 512)
{
    std::tm when = {};
    when.tm_year = 2026 - 1900;
    when.tm_mon = 9; // October
    when.tm_mday = 3;
    when.tm_hour = 9;
    when.tm_min = 5;
    when.tm_sec = 7;
    std::string buf(cap, '\0');
    const size_t n = ndi_linux_log::formatDatagram(&buf[0], buf.size(), when, 4242, msg, len);
    buf.resize(n);
    return buf;
}

int main()
{
    // RFC 3164 as glibc's syslog() writes it, so journald derives
    // SYSLOG_IDENTIFIER=NDIOutput and _PID from the tag: <PRI> with
    // LOG_USER|LOG_INFO = 14, "Mmm dd hh:mm:ss" (day space-padded), tag[pid]:.
    expectTrue(datagram("NDI Plugin: hello", 17) ==
                   "<14>Oct  3 09:05:07 NDIOutput[4242]: NDI Plugin: hello",
               "datagram carries priority, timestamp, tag[pid] and the message");

    // The message length is explicit; bytes past it are not sent.
    expectTrue(datagram("abcdef", 3) == "<14>Oct  3 09:05:07 NDIOutput[4242]: abc",
               "only len bytes of the message are used");

    // A buffer too small truncates the message, never overruns.
    const std::string small = datagram("0123456789", 10, 40);
    expectTrue(small.size() == 39 && small == std::string("<14>Oct  3 09:05:07 NDIOutput[4242]: 01"),
               "truncates to the buffer (minus the terminator)");

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all Linux log tests passed\n");
    return 0;
}
