// Tests for the process half of the Linux browse dialog
// (src/LinuxFileDialog.cpp, ticket #76): linux_open_file_dialog against fake
// zenity/kdialog shell scripts on PATH, so the real posix_spawnp, pipes and
// waitpid run without a GUI. The argv, parsing and fallback rules are
// test_linux_file_dialog's; this checks they survive the real spawn:
// argv delivered verbatim, the child environment, a host that ignores
// SIGCHLD, and a chatty helper that could fill a pipe.
// Build & run: ctest (Linux only).

#include "LinuxFileDialog.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void expectStr(const std::string& actual, const std::string& expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: '%s'\n  actual:   '%s'\n",
                     name, expected.c_str(), actual.c_str());
    } else {
        std::printf("ok   %s\n", name);
    }
}

static void expectTrue(bool actual, const char* name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

static std::string gDir; // the fake helpers' PATH directory

static void writeHelper(const char* name, const std::string& body)
{
    const std::string path = gDir + "/" + name;
    std::ofstream(path) << "#!/bin/sh\n" << body;
    chmod(path.c_str(), 0755);
}

static void removeHelpers()
{
    unlink((gDir + "/zenity").c_str());
    unlink((gDir + "/kdialog").c_str());
}

static std::string slurp(const std::string& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool browse(char* buf, size_t size, const char* initial = "/maps/Bühne 2/old.exr")
{
    return linux_open_file_dialog("Pick the left eye", "exr", initial, buf, size);
}

static void testPickAndArgv()
{
    removeHelpers();
    // One argv entry per line, so spaces and UTF-8 inside an entry show.
    writeHelper("zenity", "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"" + gDir +
                              "/argv\"\n/usr/bin/env > \"" + gDir + "/env\"\nprintf '/maps/Bühne 2/l.exr\\n'\n");
    char buf[256] = "sentinel";
    expectTrue(browse(buf, sizeof(buf)), "a fake zenity's pick returns true");
    expectStr(buf, "/maps/Bühne 2/l.exr", "the picked path arrives without its newline");
    expectStr(slurp(gDir + "/argv"),
              "--file-selection\n--modal\n--title=Pick the left eye\n"
              "--file-filter=EXR files | *.exr *.EXR\n--file-filter=All files | *\n"
              "--filename=/maps/Bühne 2/old.exr\n",
              "argv reaches the helper verbatim, one entry each");
    const std::string env = slurp(gDir + "/env");
    expectTrue(env.find("DISPLAY=:99") != std::string::npos, "the helper inherits DISPLAY");
    expectTrue(env.find("LD_LIBRARY_PATH=") == std::string::npos,
               "the helper doesn't inherit LD_LIBRARY_PATH");
    expectStr(linux_take_file_dialog_error(), "", "a pick leaves no error");
}

static void testCancel()
{
    removeHelpers();
    writeHelper("zenity", "exit 1\n");
    char buf[64] = "sentinel";
    expectTrue(!browse(buf, sizeof(buf)), "a fake zenity's cancel returns false");
    expectStr(buf, "sentinel", "a cancel leaves the buffer untouched");
    expectStr(linux_take_file_dialog_error(), "", "a cancel leaves no error");
}

static void testKdialogFallback()
{
    removeHelpers();
    writeHelper("kdialog", "printf '/maps/k.exr\\n'\n");
    char buf[64] = "sentinel";
    expectTrue(browse(buf, sizeof(buf)), "kdialog answers when zenity isn't on PATH");
    expectStr(buf, "/maps/k.exr", "kdialog's path is used");
}

static void testNoHelper()
{
    removeHelpers();
    char buf[64] = "sentinel";
    expectTrue(!browse(buf, sizeof(buf)), "no helper on PATH returns false");
    expectStr(buf, "sentinel", "no helper leaves the buffer untouched");
    const std::string error = linux_take_file_dialog_error();
    expectTrue(error.find("install the zenity package") != std::string::npos,
               "no helper names the package to install");
    expectStr(linux_take_file_dialog_error(), "", "the error is cleared by the read");
}

// 1 MB on each stream: more than both pipe buffers, so a reader that waited
// on one stream while the helper blocked on the other would hang here.
static void testChattyHelper()
{
    removeHelpers();
    writeHelper("zenity", "/usr/bin/head -c 1000000 /dev/zero | /usr/bin/tr '\\0' e >&2\n"
                          "/usr/bin/head -c 1000000 /dev/zero | /usr/bin/tr '\\0' o\nexit 3\n");
    char buf[64] = "sentinel";
    expectTrue(!browse(buf, sizeof(buf)), "a chatty failing helper returns false");
    const std::string error = linux_take_file_dialog_error();
    expectTrue(error.find("status 3") != std::string::npos, "its exit status is reported");
}

// A host with SIGCHLD set to SIG_IGN has its children reaped by the kernel,
// so waitpid fails with ECHILD; stdout still decides.
static void testSigchldIgnored()
{
    removeHelpers();
    writeHelper("zenity", "printf '/maps/ign.exr\\n'\n");
    struct sigaction old, ign;
    std::memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &ign, &old);
    char buf[64] = "sentinel";
    const bool picked = browse(buf, sizeof(buf));
    sigaction(SIGCHLD, &old, nullptr);
    expectTrue(picked, "a pick still works when the host ignores SIGCHLD");
    expectStr(buf, "/maps/ign.exr", "and the path is right");

    writeHelper("zenity", "exit 1\n");
    sigaction(SIGCHLD, &ign, &old);
    std::strcpy(buf, "sentinel");
    const bool cancelled = !browse(buf, sizeof(buf));
    sigaction(SIGCHLD, &old, nullptr);
    expectTrue(cancelled, "a cancel still reads as one when the host ignores SIGCHLD");
    expectStr(linux_take_file_dialog_error(), "", "and it isn't an error");
}

static void testNoDisplay()
{
    removeHelpers();
    writeHelper("zenity", "printf '/maps/x.exr\\n'\n");
    unsetenv("DISPLAY");
    char buf[64] = "sentinel";
    expectTrue(!browse(buf, sizeof(buf)), "no display returns false");
    expectTrue(linux_take_file_dialog_error().find("DISPLAY") != std::string::npos,
               "no display says so");
    setenv("DISPLAY", ":99", 1);
}

// The idle hook (the host event pump in the plugin) runs about every 30 ms
// while the helper is alive, so Resolve repaints during a long pick.
static void testIdleWhileWaiting()
{
    removeHelpers();
    writeHelper("zenity", "/usr/bin/sleep 0.5\nprintf '/maps/slow.exr\\n'\n");
    int idles = 0;
    const ndi_linuxdlg::SpawnResult r = ndi_linuxdlg::spawnProcess(
        {"zenity", "--file-selection"}, [&] { ++idles; return true; });
    expectTrue(r.status == ndi_linuxdlg::SpawnResult::kExited && r.exitCode == 0,
               "a slow helper still exits 0");
    expectStr(r.out, "/maps/slow.exr\n", "and its stdout arrives whole");
    expectTrue(idles >= 8 && idles <= 40, "the idle hook ran repeatedly during a 0.5 s pick");
    std::printf("     (%d idle calls in 0.5 s)\n", idles);

    idles = 0;
    ndi_linuxdlg::spawnProcess({"no-such-helper"}, [&] { ++idles; return true; });
    expectTrue(idles == 0, "no idle calls when nothing was spawned");
}

// idle returning false (the host is going away) closes the helper: SIGTERM,
// and SIGKILL for one that ignores it. The result is marked aborted, so the
// dialog reads it as a cancel.
static void testAbort()
{
    removeHelpers();
    writeHelper("zenity", "/usr/bin/sleep 30\nprintf '/maps/late.exr\\n'\n");
    int idles = 0;
    auto start = std::chrono::steady_clock::now();
    ndi_linuxdlg::SpawnResult r = ndi_linuxdlg::spawnProcess(
        {"zenity", "--file-selection"}, [&] { return ++idles < 3; });
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expectTrue(r.aborted, "idle returning false aborts the helper");
    expectTrue(r.out.empty(), "before it printed anything");
    expectTrue(secs < 1.5, "and SIGTERM closes it promptly");
    expectTrue(idles == 3, "idle isn't called again after it said stop");
    expectTrue(ndi_linuxdlg::interpret("zenity", r).outcome == ndi_linuxdlg::Outcome::kCancelled,
               "an aborted helper is a cancel");

    // A helper that ignores SIGTERM: killed after the 2 s grace.
    removeHelpers();
    writeHelper("zenity", "trap '' TERM\n/usr/bin/sleep 30 & wait $!; /usr/bin/sleep 30\n");
    start = std::chrono::steady_clock::now();
    r = ndi_linuxdlg::spawnProcess({"zenity"}, [] { return false; });
    secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expectTrue(r.aborted, "a TERM-proof helper is aborted too");
    expectTrue(secs >= 1.9 && secs < 6.0, "SIGKILL after the grace (and the pipes given up on)");
    std::printf("     (TERM-proof helper closed in %.1f s)\n", secs);
}

int main()
{
    char tmpl[] = "/tmp/ndi_lfd_XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::perror("mkdtemp");
        return 1;
    }
    gDir = tmpl;
    // PATH is the fake helpers' directory alone, so a real zenity or kdialog
    // on the machine can't answer instead; the scripts name everything else
    // by absolute path (sh, printf and exit are the shell's own).
    setenv("PATH", gDir.c_str(), 1);
    setenv("DISPLAY", ":99", 1);
    unsetenv("WAYLAND_DISPLAY");
    setenv("LD_LIBRARY_PATH", "/opt/resolve/libs", 1);
    setenv("HOME", "/home/test", 1);

    testPickAndArgv();
    testCancel();
    testKdialogFallback();
    testNoHelper();
    testChattyHelper();
    testSigchldIgnored();
    testNoDisplay();
    testIdleWhileWaiting();
    testAbort();

    removeHelpers();
    unlink((gDir + "/argv").c_str());
    unlink((gDir + "/env").c_str());
    rmdir(gDir.c_str());

    if (failures) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
