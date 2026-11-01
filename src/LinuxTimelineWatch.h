// Discovery seam for the Linux timeline-clip watcher (ticket #75, spec
// decision 17): the pure pieces of "find Resolve's own script interpreter,
// the scripting API beside it, the helper scripts in our bundle, and the
// environment the Python fallback needs". Platform-neutral string work, so
// the unit suite (tests/test_linux_timeline_watch.cpp) runs it on every CI
// platform, mirroring src/MacTimelineWatch.h and src/WinTimelineWatch.h.
// The posix_spawn plumbing is src/LinuxTimelineSpawn.h; the watcher itself
// — reader thread, respawn/backoff, the line protocol — is
// src/LinuxTimelineWatch.cpp, implementing the TimelineClipWatcher.h API.

#ifndef LINUX_TIMELINE_WATCH_H
#define LINUX_TIMELINE_WATCH_H

#include <string>
#include <vector>

namespace ndi_timelinewatch_linux {

// The stock install root, for a host whose executable path can't be read or
// is too shallow (a standalone test harness).
constexpr const char* kDefaultResolveRoot = "/opt/resolve";

// ---- Resolve install root ------------------------------------------------------
// Resolve's binary is <root>/bin/resolve (/proc/self/exe -> /opt/resolve/bin/
// resolve on the box; this code runs inside Resolve's process). Hop two
// components up. Empty when the path is too shallow to hop.
inline std::string resolveRootForHostExecutable(const std::string& hostExecutable)
{
    const size_t last = hostExecutable.find_last_of('/');
    if (last == std::string::npos || last == 0) {
        return std::string();
    }
    const size_t bin = hostExecutable.find_last_of('/', last - 1);
    if (bin == std::string::npos) {
        return std::string();
    }
    return bin == 0 ? std::string("/") : hostExecutable.substr(0, bin);
}

namespace detail {
// root + "/" + rel, without a doubled slash; empty when root is.
inline std::string underRoot(std::string root, const char* rel)
{
    if (root.empty()) {
        return std::string();
    }
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    return root + "/" + rel;
}
} // namespace detail

// ---- Paths beside the root (docs/2026-10-30-linux-host-probes.md) ---------------
// Resolve's bundled interpreter: the primary helper runs the Lua script
// under it, so Auto depends on nothing outside Resolve.
inline std::string fuscriptPath(const std::string& root)
{
    return detail::underRoot(root, "libs/Fusion/fuscript");
}

// The CPython extension DaVinciResolveScript loads (RESOLVE_SCRIPT_LIB).
inline std::string scriptingLibPath(const std::string& root)
{
    return detail::underRoot(root, "libs/Fusion/fusionscript.so");
}

// RESOLVE_SCRIPT_API: the scripting README and examples.
inline std::string scriptingApiDir(const std::string& root)
{
    return detail::underRoot(root, "Developer/Scripting");
}

// DaVinciResolveScript.py, which the Python helper imports (PYTHONPATH).
inline std::string scriptingModulesDir(const std::string& root)
{
    return detail::underRoot(root, "Developer/Scripting/Modules");
}

// ---- Helper scripts in the bundle ----------------------------------------------
// dladdr names the .ofx as Resolve dlopened it: <bundle>/Contents/
// Linux-x86-64/NDIOutput.ofx. The helpers ride in <bundle>/Contents/
// Resources. Pure string hop (the caller realpath()s the result to check it
// exists); empty when the module path is too shallow.
inline std::string bundleResourcePath(const std::string& modulePath, const std::string& name)
{
    const size_t file = modulePath.find_last_of('/');
    if (file == std::string::npos || file == 0) {
        return std::string();
    }
    const size_t arch = modulePath.find_last_of('/', file - 1);
    if (arch == std::string::npos) {
        return std::string();
    }
    return modulePath.substr(0, arch + 1) + "Resources/" + name;
}

// ---- Python fallback environment ------------------------------------------------
// Only reached when fuscript is missing. /usr/bin/python3 is a real
// interpreter on Linux (no macOS shim, no Store stub), but it knows nothing
// about Resolve: the child gets Resolve's documented scripting variables
// (Developer/Scripting/README.txt), derived from the running install so a
// relocated Resolve works. Built from `parent` (environ as "KEY=VALUE"):
// RESOLVE_SCRIPT_API/RESOLVE_SCRIPT_LIB replaced, the Modules dir put first
// on PYTHONPATH (an inherited PYTHONPATH kept behind it), and PYTHONHOME
// dropped — it would point the system python at some other interpreter's
// stdlib. Everything else passes through.
inline std::vector<std::string> pythonHelperEnvironment(const std::vector<std::string>& parent,
                                                        const std::string& root)
{
    auto keyIs = [](const std::string& entry, const char* key) {
        const size_t n = std::char_traits<char>::length(key);
        return entry.size() > n && entry.compare(0, n, key) == 0 && entry[n] == '=';
    };
    std::vector<std::string> out;
    std::string pythonPath = scriptingModulesDir(root);
    for (const std::string& entry : parent) {
        if (keyIs(entry, "RESOLVE_SCRIPT_API") || keyIs(entry, "RESOLVE_SCRIPT_LIB") ||
            keyIs(entry, "PYTHONHOME")) {
            continue;
        }
        if (keyIs(entry, "PYTHONPATH")) {
            const std::string inherited = entry.substr(sizeof("PYTHONPATH"));
            if (!inherited.empty()) {
                pythonPath += ":" + inherited;
            }
            continue;
        }
        out.push_back(entry);
    }
    out.push_back("RESOLVE_SCRIPT_API=" + scriptingApiDir(root));
    out.push_back("RESOLVE_SCRIPT_LIB=" + scriptingLibPath(root));
    out.push_back("PYTHONPATH=" + pythonPath);
    return out;
}

// ---- Descriptors the helper must not inherit ----------------------------------------
// Resolve opens its listening sockets (scripting, NDI) without close-on-exec,
// and a helper holding them outlives Resolve and wedges the next launch's
// scripting (LEARNINGS 2026-10-29). macOS has POSIX_SPAWN_CLOEXEC_DEFAULT;
// Linux glibc >= 2.34 has posix_spawn_file_actions_addclosefrom_np, and
// older glibc (the Rocky 8 baseline) gets one close action per descriptor
// listed in /proc/self/fd. This filters that listing: the numeric names from
// 3 up, in the order given ("." / ".." and junk skipped).
inline std::vector<int> descriptorsToClose(const std::vector<std::string>& procFdNames)
{
    std::vector<int> out;
    for (const std::string& name : procFdNames) {
        if (name.empty() || name.size() > 9) {
            continue;  // not a descriptor (9 digits is far past any fd limit)
        }
        int fd = 0;
        bool digits = true;
        for (char c : name) {
            if (c < '0' || c > '9') {
                digits = false;
                break;
            }
            fd = fd * 10 + (c - '0');
        }
        if (digits && fd >= 3) {
            out.push_back(fd);
        }
    }
    return out;
}

} // namespace ndi_timelinewatch_linux

#endif // LINUX_TIMELINE_WATCH_H
