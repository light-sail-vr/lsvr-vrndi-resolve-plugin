// Tests for the Linux timeline-watch spawn seam (src/LinuxTimelineSpawn.h,
// ticket #75): posix_spawn with the child's stdout on a pipe, smoke-tested
// with /bin/sh — lines arrive intact, stderr stays out of the protocol, the
// exit status reaches the log wording, the child inherits none of the host's
// descriptors (under both close strategies) and none of its blocked or
// ignored signals, and the Python fallback's environment reaches the child.
// Linux only (CMakeLists.txt); the pure discovery half is
// tests/test_linux_timeline_watch.cpp.

#include "LinuxTimelineSpawn.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

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

using ndi_timelinewatch_linux::CloseStrategy;
using ndi_timelinewatch_linux::describeWaitStatus;
using ndi_timelinewatch_linux::spawnPipedProcess;

// Run argv to completion, returning its stdout and wait status.
static std::string runForOutput(const std::vector<std::string>& argv, int* waitStatus,
                                const std::vector<std::string>* env = nullptr,
                                CloseStrategy strategy = CloseStrategy::Auto)
{
    pid_t pid = -1;
    int fd = -1;
    std::string error;
    std::string out;
    *waitStatus = -1;
    if (!spawnPipedProcess(argv, env, &pid, &fd, &error, strategy)) {
        return "spawn failed: " + error;
    }
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    waitpid(pid, waitStatus, 0);
    return out;
}

static void testDescribeWaitStatus()
{
    // The exit code is what identifies a dead interpreter (issue #34), so it
    // must reach the log verbatim; a signal death says which signal.
    expectTrue(describeWaitStatus(0) == "code 0", "clean exit reads as code 0");
    expectTrue(describeWaitStatus(127 << 8) == "code 127", "exit(127) (exec failure) reads as code 127");
    expectTrue(describeWaitStatus(15) == "signal 15", "SIGTERM death reads as signal 15");
    expectTrue(describeWaitStatus(9) == "signal 9", "SIGKILL death reads as signal 9");
}

static void testSpawnPipedProcess()
{
    pid_t pid = -1;
    int fd = -1;
    std::string error;
    const std::vector<std::string> argv = {
        "/bin/sh", "-c", "printf '!status\\n/media/clip.braw\\n'; echo noise 1>&2; exit 7"};
    expectTrue(spawnPipedProcess(argv, nullptr, &pid, &fd, &error),
               ("spawn succeeds" + (error.empty() ? "" : " (" + error + ")")).c_str());
    if (pid <= 0) {
        return;
    }
    // Close-on-exec on our read end: any OTHER child Resolve forks meanwhile
    // must not hold the pipe open.
    expectTrue((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "read end is close-on-exec");
    FILE* stream = fdopen(fd, "r");
    bool sawStatus = false, sawPath = false, sawNoise = false;
    if (stream) {
        char line[256];
        while (fgets(line, sizeof(line), stream) != nullptr) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s == "!status") sawStatus = true;
            if (s == "/media/clip.braw") sawPath = true;
            if (s == "noise") sawNoise = true;
        }
        fclose(stream);
    } else {
        close(fd);
    }
    expectTrue(sawStatus && sawPath, "lines arrive intact through a FILE*");
    expectTrue(!sawNoise, "stderr does not leak into the line protocol");
    int status = 0;
    expectTrue(waitpid(pid, &status, 0) == pid, "child is reapable after EOF");
    expectTrue(describeWaitStatus(status) == "code 7", "exit code survives to the log wording");

    pid = -1;
    fd = -1;
    error.clear();
    expectTrue(!spawnPipedProcess({"/nonexistent/fuscript", "-q"}, nullptr, &pid, &fd, &error),
               "missing executable fails");
    expectTrue(!error.empty() && pid == -1 && fd == -1,
               "failure reports an error and leaves no pid/fd behind");
}

// Resolve's sockets are inheritable; a helper that keeps them outlives
// Resolve and wedges the next launch's scripting (LEARNINGS 2026-10-29).
// Model one with a listening socket on fd 9 (single digit so /bin/sh can
// name it) and a plain fd on 200, and ask the child what it can see.
static void testSpawnInheritsNoHostDescriptors(CloseStrategy strategy, const char* label)
{
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool listening = sock >= 0 &&
        bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        listen(sock, 1) == 0 && dup2(sock, 9) == 9 && dup2(sock, 200) == 200;
    if (sock >= 0 && sock != 9) {
        close(sock);
    }
    expectTrue(listening && (fcntl(9, F_GETFD) & FD_CLOEXEC) == 0,
               "inheritable listening socket on fds 9 and 200");
    if (!listening) {
        return;
    }
    int status = 0;
    // ls lists its own directory fd too, so look for the two by number.
    const std::string out = runForOutput(
        {"/bin/sh", "-c",
         "for f in 9 200; do [ -e /proc/self/fd/$f ] && echo leaked $f; done; echo done"},
        &status, nullptr, strategy);
    close(9);
    close(200);
    expectTrue(out == "done\n", (std::string("helper inherits no host descriptors (") + label +
                                 ", child said: " + out.substr(0, out.find('\n')) + ")")
                                    .c_str());
}

static void testSpawnResetsSignalState()
{
    // posix_spawn keeps the calling thread's signal mask and every ignored
    // disposition across exec. Resolve's threads may block SIGTERM, and a
    // helper with SIGTERM blocked shrugs off the watcher's shutdown kill;
    // with SIGCHLD ignored, fuscript can't reap its own children. The child
    // must start with an empty mask and default dispositions.
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    struct sigaction ign = {}, oldPipe = {};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &oldPipe);

    int status = 0;
    const std::string out = runForOutput({"/bin/sh", "-c", "kill -TERM $$; echo survived"}, &status);
    expectTrue(out.empty() && describeWaitStatus(status) == "signal 15",
               ("a blocked SIGTERM in the parent doesn't protect the child (" +
                describeWaitStatus(status) + ")").c_str());
    const std::string ignored = runForOutput(
        {"/bin/sh", "-c", "grep -E '^Sig(Blk|Ign)' /proc/self/status"}, &status);
    // sh may ignore a few signals itself (none it does by default under -c);
    // what matters is SIGPIPE (bit 13) and SIGTERM (bit 15) are clear.
    expectTrue(ignored.find("SigBlk:\t0000000000000000") != std::string::npos,
               "the child's signal mask is empty");
    expectTrue(ignored.find("SigIgn:\t0000000000000000") != std::string::npos,
               ("the child ignores no signals (" + ignored.substr(ignored.find("SigIgn")) + ")").c_str());

    sigaction(SIGPIPE, &oldPipe, nullptr);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
}

static void testSpawnEnvironment()
{
    // The Python fallback's variables reach the child; nullptr inherits ours.
    const std::vector<std::string> env = {"RESOLVE_SCRIPT_LIB=/opt/resolve/libs/Fusion/fusionscript.so",
                                          "PATH=/usr/bin:/bin"};
    int status = 0;
    const std::string out =
        runForOutput({"/bin/sh", "-c", "echo \"$RESOLVE_SCRIPT_LIB|$HOME\""}, &status, &env);
    expectTrue(out == "/opt/resolve/libs/Fusion/fusionscript.so|\n",
               "an explicit environment replaces the parent's");
    setenv("NDI_SPAWN_TEST_MARK", "inherited", 1);
    const std::string inherited =
        runForOutput({"/bin/sh", "-c", "echo $NDI_SPAWN_TEST_MARK"}, &status);
    expectTrue(inherited == "inherited\n", "no environment given inherits the parent's");
}

static void testExitsCleanly()
{
    using ndi_timelinewatch_linux::exitsCleanly;
    expectTrue(exitsCleanly({"/bin/sh", "-c", "echo Python 3.12.3; exit 0"}),
               "a candidate that exits 0 is trusted");
    expectTrue(!exitsCleanly({"/bin/sh", "-c", "exit 1"}), "a candidate that exits non-zero is rejected");
    expectTrue(!exitsCleanly({"/nonexistent/python3", "--version"}),
               "a candidate that cannot spawn is rejected");

    // The probe runs under the environment it is given — the helper's own —
    // not the parent's: a candidate that only works with Resolve's
    // LD_LIBRARY_PATH must fail the probe, not pass it and die as the helper.
    setenv("LD_LIBRARY_PATH", "/opt/resolve/libs", 1);
    std::vector<std::string> parent;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        parent.emplace_back(*e);
    }
    unsetenv("LD_LIBRARY_PATH");
    const std::vector<std::string> helperEnv =
        ndi_timelinewatch_linux::pythonHelperEnvironment(parent, "/opt/resolve");
    const std::vector<std::string> needsResolveLibs = {"/bin/sh", "-c",
                                                       "[ \"$LD_LIBRARY_PATH\" = /opt/resolve/libs ]"};
    const std::vector<std::string> needsHelperEnv = {
        "/bin/sh", "-c",
        "[ -z \"$LD_LIBRARY_PATH\" ] && [ \"$RESOLVE_SCRIPT_LIB\" = "
        "/opt/resolve/libs/Fusion/fusionscript.so ]"};
    expectTrue(!exitsCleanly(needsResolveLibs, &helperEnv),
               "the probe does not see the parent's LD_LIBRARY_PATH");
    expectTrue(exitsCleanly(needsHelperEnv, &helperEnv), "the probe runs under the helper's environment");
}

int main()
{
    std::printf("addclosefrom_np available: %s\n",
                ndi_timelinewatch_linux::haveAddClosefrom() ? "yes" : "no (/proc/self/fd fallback)");
    testDescribeWaitStatus();
    testSpawnPipedProcess();
    testSpawnInheritsNoHostDescriptors(CloseStrategy::Auto, "auto");
    testSpawnInheritsNoHostDescriptors(CloseStrategy::ProcFdList, "/proc/self/fd list");
    testSpawnResetsSignalState();
    testSpawnEnvironment();
    testExitsCleanly();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all linux timeline-spawn tests passed\n");
    return 0;
}
