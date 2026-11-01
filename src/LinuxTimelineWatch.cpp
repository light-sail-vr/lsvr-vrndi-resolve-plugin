// Linux implementation of src/TimelineClipWatcher.h (ticket #75, spec
// decision 17) — the same one-helper-process-plus-one-reader-thread design
// as the macOS implementation (src/TimelineClipWatcher.cpp), ported rather
// than shared so macOS code stays untouched. Discovery lives in
// src/LinuxTimelineWatch.h (Resolve root from /proc/self/exe, fuscript and
// the scripting API beside it, the bundle's Resources from dladdr), the
// posix_spawn plumbing in src/LinuxTimelineSpawn.h; both are unit-tested.
// The helper scripts and their line protocol are shared verbatim.
#ifdef __linux__

#include "TimelineClipWatcher.h"

#include "LinuxLog.h"
#include "LinuxTimelineSpawn.h"
#include "LinuxTimelineWatch.h"

#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ndi_timelinewatch {

namespace {

// Same sink and prefix as the plugin's NDI_LOG on Linux (journald tag
// NDIOutput, plus NDI_OUTPUT_LOG_FILE).
void watchLog(const std::string& msg)
{
    ndi_linux_log::write("NDI Plugin: TimelineWatch: %s", msg.c_str());
}

constexpr int kRespawnBackoffSeconds = 30;

std::mutex gMutex;                 // guards everything below
bool gStarted = false;
std::atomic<bool> gStop{false};
std::atomic<bool> gReaderDone{false};  // readerLoop has returned
std::thread gThread;
pid_t gHelperPid = -1;
int gHelperFd = -1;                // read end of the helper's stdout pipe
std::function<void(const std::string&)> gOnChange;
std::string gCurrentPath;
bool gHaveReport = false;
std::function<void()> gOnColor;    // timeline colour listener (issue #57)
ndi_tlcolor::Settings gColor;      // latest "@color" report
bool gHaveColor = false;
std::string gHealthDetail = "not started";

// <bundle>/Contents/Resources/<name> for the .ofx this code is linked into,
// as Resolve dlopened it. Empty when it can't be found on disk.
std::string bundleResource(const char* name)
{
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&bundleResource), &info) == 0 ||
        info.dli_fname == nullptr) {
        return std::string();
    }
    const std::string candidate = ndi_timelinewatch_linux::bundleResourcePath(info.dli_fname, name);
    char resolved[PATH_MAX];
    if (!candidate.empty() && realpath(candidate.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
    return std::string();
}

// Resolve's executable (this code runs inside its process), the anchor for
// finding the install root. Empty on failure.
std::string hostExecutablePath()
{
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf, static_cast<size_t>(n));
}

bool isExecutableFile(const std::string& path)
{
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           access(path.c_str(), X_OK) == 0;
}

// The Resolve install this process runs from (/opt/resolve on a stock
// install), or the stock root when the host executable is no Resolve (a
// standalone harness) or has no fuscript beside it.
std::string resolveRoot()
{
    const std::string fromHost =
        ndi_timelinewatch_linux::resolveRootForHostExecutable(hostExecutablePath());
    if (isExecutableFile(ndi_timelinewatch_linux::fuscriptPath(fromHost))) {
        return fromHost;
    }
    return ndi_timelinewatch_linux::kDefaultResolveRoot;
}

void setHealthLocked(bool ok, const std::string& detail)
{
    gHaveReport = ok;
    gHealthDetail = detail;
}

// Spawn the helper with its stdout on a pipe. Returns false with a health
// detail already set (caller holds gMutex).
bool spawnHelperLocked()
{
    using namespace ndi_timelinewatch_linux;

    std::vector<std::string> argv;
    std::vector<std::string> env;
    const std::vector<std::string>* childEnv = nullptr;  // inherit Resolve's
    std::string launchSummary;
    const std::string root = resolveRoot();
    const std::string fuscript = fuscriptPath(root);
    const std::string luaScript = bundleResource("ndi_timeline_watch.lua");
    if (isExecutableFile(fuscript) && !luaScript.empty()) {
        // -q suppresses the banner fuscript writes to stdout, which the line
        // protocol would otherwise read as clip paths; -l lua selects the
        // interpreter (same flags as macOS and Windows).
        argv = {fuscript, "-q", "-l", "lua", luaScript};
        launchSummary = "fuscript [" + fuscript + "] " + luaScript;
    } else {
        // Fallback: the Python helper under the system python3, with
        // Resolve's scripting variables set for the child.
        const std::string script = bundleResource("ndi_timeline_watch.py");
        if (script.empty()) {
            setHealthLocked(false, "helper scripts missing from the plugin bundle's Resources");
            return false;
        }
        static const char* kPython = "/usr/bin/python3";
        if (!isExecutableFile(kPython) || !exitsCleanly({kPython, "--version"})) {
            setHealthLocked(false, "no fuscript at " + fuscript +
                                       " and no working /usr/bin/python3 — reinstall DaVinci "
                                       "Resolve, install python3, or set a Manual Path clip");
            return false;
        }
        std::vector<std::string> parent;
        for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
            parent.emplace_back(*e);
        }
        env = pythonHelperEnvironment(parent, root);
        childEnv = &env;
        argv = {kPython, script};
        launchSummary = "python [" + std::string(kPython) + ", RESOLVE_SCRIPT_LIB=" +
                        scriptingLibPath(root) + "] " + script;
    }

    pid_t pid = -1;
    int fd = -1;
    std::string error;
    if (!spawnPipedProcess(argv, childEnv, &pid, &fd, &error)) {
        setHealthLocked(false, "helper spawn failed: " + error);
        return false;
    }
    gHelperPid = pid;
    gHelperFd = fd;
    setHealthLocked(false, "helper starting");  // healthy once the first line lands
    watchLog("helper started (" + launchSummary + ")");
    return true;
}

// NOTE fd ownership: only the reader thread ever closes gHelperFd (via
// fclose of the FILE* wrapping it). Other threads must not close it — the
// reader may be blocked in fgets on that fd, and a cross-thread close could
// double-close an fd the host has since reused. Shutdown instead SIGTERMs
// the helper; its death EOFs the pipe and unblocks the reader within one
// heartbeat (~0.5 s).

// Reader thread: consume helper lines, dedupe, fan out changes; respawn the
// helper (with backoff) whenever it dies, until shutdown.
void readerLoop()
{
    struct DoneFlag {
        ~DoneFlag() { gReaderDone.store(true); }
    } doneFlag;
    std::string lastAnnouncedError;
    while (!gStop.load(std::memory_order_relaxed)) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (gHelperFd < 0 && !spawnHelperLocked()) {
                if (gHealthDetail != lastAnnouncedError) {
                    lastAnnouncedError = gHealthDetail;
                    watchLog(gHealthDetail);
                }
            }
            fd = gHelperFd;
        }
        if (fd < 0) {
            for (int i = 0; i < kRespawnBackoffSeconds * 10 && !gStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        FILE* stream = fdopen(fd, "r");
        if (stream == nullptr) {
            std::lock_guard<std::mutex> lock(gMutex);
            close(gHelperFd);
            gHelperFd = -1;
            if (gHelperPid > 0) {
                kill(gHelperPid, SIGTERM);
                int status = 0;
                waitpid(gHelperPid, &status, 0);
                gHelperPid = -1;
            }
            continue;
        }
        char line[4096];
        while (!gStop.load(std::memory_order_relaxed) &&
               fgets(line, sizeof(line), stream) != nullptr) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (line[0] == '!') {
                // Status message from the helper — log deduped.
                std::lock_guard<std::mutex> lock(gMutex);
                const std::string msg(line + 1);
                if (msg != lastAnnouncedError) {
                    lastAnnouncedError = msg;
                    watchLog(msg);
                }
                continue;
            }
            if (line[0] == '@') {
                // Timeline colour settings (issue #57 Part 3) — deduped,
                // fanned out to the colour listener. Other '@' tags are
                // ignored, so a newer helper never reads as a clip path.
                ndi_tlcolor::Settings settings;
                if (!ndi_tlcolor::parseColorLine(line, &settings)) {
                    continue;
                }
                std::function<void()> onColor;
                {
                    std::lock_guard<std::mutex> lock(gMutex);
                    if (gHaveColor && gColor == settings) {
                        continue;
                    }
                    gColor = settings;
                    gHaveColor = true;
                    onColor = gOnColor;
                }
                watchLog("timeline colour: " + settings.scienceMode + ", timeline '" +
                         settings.timelineSpace + "' / '" + settings.timelineGamma +
                         "', output '" + settings.outputSpace + "' / '" + settings.outputGamma +
                         "'");
                if (onColor) {
                    onColor();
                }
                continue;
            }
            std::function<void(const std::string&)> onChange;
            std::string changed;
            bool fire = false;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (!gHaveReport || gCurrentPath != line) {
                    gCurrentPath = line;
                    changed = gCurrentPath;
                    onChange = gOnChange;
                    fire = true;
                }
                setHealthLocked(true, "watching");
            }
            if (fire) {
                watchLog(changed.empty() ? std::string("playhead clip: (none)")
                                         : "playhead clip: '" + changed + "'");
                if (onChange) {
                    onChange(changed);
                }
            }
        }
        // EOF (helper died) or shutdown. fclose also closes the fd.
        fclose(stream);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gHelperFd = -1;
            // The exit status is how a dying helper identifies itself — an
            // interpreter that dies before its first protocol line leaves no
            // other evidence in the log (issue #34).
            std::string exitDetail = "status unknown";
            if (gHelperPid > 0) {
                int status = 0;
                if (waitpid(gHelperPid, &status, 0) == gHelperPid) {
                    exitDetail = ndi_timelinewatch_linux::describeWaitStatus(status);
                }
                gHelperPid = -1;
            }
            if (!gStop.load(std::memory_order_relaxed)) {
                setHealthLocked(false, "helper exited (" + exitDetail + ") — retrying");
                watchLog("helper exited (" + exitDetail + ") — retrying in 30 s (Manual "
                         "Path mode is unaffected)");
            }
        }
        for (int i = 0; i < kRespawnBackoffSeconds * 10 && !gStop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace

void ensureStarted(std::function<void(const std::string&)> onChange)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (gStarted) {
        return;
    }
    gStarted = true;
    gStop.store(false);
    gReaderDone.store(false);
    gOnChange = std::move(onChange);
    gThread = std::thread(readerLoop);
}

std::string currentClipPath()
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gCurrentPath;
}

void setColorListener(std::function<void()> onColorChange)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gOnColor) {
        gOnColor = std::move(onColorChange);
    }
}

bool currentTimelineColor(ndi_tlcolor::Settings* out)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (gHaveColor && out) {
        *out = gColor;
    }
    return gHaveColor;
}

bool healthy(std::string* detail)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (detail) {
        *detail = gHealthDetail;
    }
    return gHaveReport;
}

void shutdown()
{
    pid_t helper = -1;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gStarted) {
            return;
        }
        gStop.store(true);
        // Killing the helper EOFs the pipe, which unblocks the reader's
        // fgets within one heartbeat; the reader owns the fd (see above).
        helper = gHelperPid;
        if (helper > 0) {
            kill(helper, SIGTERM);
        }
    }
    // A helper that traps SIGTERM would otherwise pin the reader in fgets
    // and this join in plugin unload forever: escalate to SIGKILL after 2 s
    // (as macOS and Windows do). The reader still reaps it; a pid that
    // already died and was reaped is never re-signalled (gHelperPid is
    // cleared under the mutex before any reuse).
    for (int i = 0; i < 20 && !gReaderDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!gReaderDone.load()) {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gHelperPid > 0 && gHelperPid == helper) {
            kill(gHelperPid, SIGKILL);
        }
    }
    if (gThread.joinable()) {
        gThread.join();
    }
    std::lock_guard<std::mutex> lock(gMutex);
    if (gHelperPid > 0) {  // reader exited before reaping (spawn-window race)
        int status = 0;
        waitpid(gHelperPid, &status, 0);
        gHelperPid = -1;
    }
    gStarted = false;
    gOnChange = nullptr;
    gOnColor = nullptr;
    gHaveColor = false;
    setHealthLocked(false, "stopped");
}

} // namespace ndi_timelinewatch

#endif // __linux__
