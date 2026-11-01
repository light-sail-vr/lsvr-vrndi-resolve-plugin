// Tests for the Linux timeline-watch discovery seam (src/LinuxTimelineWatch.h,
// ticket #75): Resolve's install root from the host executable, fuscript and
// the scripting API beside it, the Python fallback's child environment, the
// helper scripts in the bundle's Resources, and the /proc/self/fd filter the
// spawn falls back on. Pure string work, so it runs in ctest on every
// platform (Linux and Windows CI). The posix_spawn plumbing itself is
// tests/test_linux_timeline_spawn.cpp (Linux only).

#include "LinuxTimelineWatch.h"

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

static void testResolveRootForHostExecutable()
{
    using ndi_timelinewatch_linux::resolveRootForHostExecutable;

    // The stock install, read from /proc/self/exe on COMFYBOX (Resolve
    // Studio 21.1, 2026-11-01): the binary is <root>/bin/resolve.
    expectTrue(resolveRootForHostExecutable("/opt/resolve/bin/resolve") == "/opt/resolve",
               "stock install: /opt/resolve/bin/resolve -> /opt/resolve");
    // A relocated install keeps the same internal layout.
    expectTrue(resolveRootForHostExecutable("/home/x/apps/Resolve 21/bin/resolve") ==
                   "/home/x/apps/Resolve 21",
               "relocated install keeps the layout");
    // Too shallow to hop from <root>/bin up to <root>: empty (the caller
    // falls back to the stock /opt/resolve).
    expectTrue(resolveRootForHostExecutable("resolve").empty(), "bare name yields empty");
    expectTrue(resolveRootForHostExecutable("bin/resolve").empty(), "relative one-level path yields empty");
    expectTrue(resolveRootForHostExecutable("/resolve").empty(), "a binary at / yields empty");
    expectTrue(resolveRootForHostExecutable("").empty(), "empty path yields empty");
    // /bin/resolve: the root would be "/" itself — still usable, spelled
    // without a doubled slash downstream.
    expectTrue(resolveRootForHostExecutable("/bin/resolve") == "/", "/bin/resolve -> /");
}

static void testPathsBesideTheRoot()
{
    using namespace ndi_timelinewatch_linux;

    // Paths from #71's host probes (docs/2026-10-30-linux-host-probes.md,
    // LEARNINGS "Linux environment map").
    expectTrue(fuscriptPath("/opt/resolve") == "/opt/resolve/libs/Fusion/fuscript",
               "fuscript is <root>/libs/Fusion/fuscript");
    expectTrue(scriptingLibPath("/opt/resolve") == "/opt/resolve/libs/Fusion/fusionscript.so",
               "fusionscript.so sits beside fuscript");
    expectTrue(scriptingApiDir("/opt/resolve") == "/opt/resolve/Developer/Scripting",
               "scripting API is <root>/Developer/Scripting");
    expectTrue(scriptingModulesDir("/opt/resolve") == "/opt/resolve/Developer/Scripting/Modules",
               "DaVinciResolveScript.py lives in <root>/Developer/Scripting/Modules");
    expectTrue(fuscriptPath("/") == "/libs/Fusion/fuscript", "root '/' gives no doubled slash");
    expectTrue(fuscriptPath("/opt/resolve/") == "/opt/resolve/libs/Fusion/fuscript",
               "a trailing slash on the root is tolerated");
    expectTrue(fuscriptPath("").empty() && scriptingApiDir("").empty() &&
                   scriptingLibPath("").empty() && scriptingModulesDir("").empty(),
               "no root, no paths");
}

static void testBundleResourcePath()
{
    using ndi_timelinewatch_linux::bundleResourcePath;

    // dladdr names the .ofx as Resolve dlopened it: the bundle's
    // Contents/Linux-x86-64/NDIOutput.ofx (spec decision 7). The helpers ride
    // in Contents/Resources (CMakeLists.txt install rule).
    expectTrue(bundleResourcePath("/usr/OFX/Plugins/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx",
                                  "ndi_timeline_watch.lua") ==
                   "/usr/OFX/Plugins/NDIOutput.ofx.bundle/Contents/Resources/ndi_timeline_watch.lua",
               "installed bundle: Contents/Linux-x86-64 -> Contents/Resources");
    expectTrue(bundleResourcePath("/home/matt/ndioutput-75/build/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx",
                                  "ndi_timeline_watch.py") ==
                   "/home/matt/ndioutput-75/build/NDIOutput.ofx.bundle/Contents/Resources/ndi_timeline_watch.py",
               "a staged bundle anywhere works the same");
    expectTrue(bundleResourcePath("NDIOutput.ofx", "x.lua").empty(), "bare module name yields empty");
    expectTrue(bundleResourcePath("Linux-x86-64/NDIOutput.ofx", "x.lua").empty(),
               "a module one level deep yields empty");
    expectTrue(bundleResourcePath("", "x.lua").empty(), "unknown module yields empty");
}

static bool hasEntry(const std::vector<std::string>& env, const std::string& entry)
{
    for (const std::string& e : env) {
        if (e == entry) return true;
    }
    return false;
}

static int countKey(const std::vector<std::string>& env, const std::string& key)
{
    int n = 0;
    for (const std::string& e : env) {
        if (e.compare(0, key.size() + 1, key + "=") == 0) ++n;
    }
    return n;
}

static void testPythonHelperEnvironment()
{
    using ndi_timelinewatch_linux::pythonHelperEnvironment;

    // The Python fallback runs /usr/bin/python3 (a real interpreter on
    // Linux) with Resolve's documented scripting variables set for the
    // child, derived from the running install rather than hardcoded
    // (Developer/Scripting/README.txt, Linux section).
    {
        const std::vector<std::string> env =
            pythonHelperEnvironment({"HOME=/home/matt", "DISPLAY=:0"}, "/opt/resolve");
        expectTrue(hasEntry(env, "RESOLVE_SCRIPT_API=/opt/resolve/Developer/Scripting"),
                   "RESOLVE_SCRIPT_API set from the root");
        expectTrue(hasEntry(env, "RESOLVE_SCRIPT_LIB=/opt/resolve/libs/Fusion/fusionscript.so"),
                   "RESOLVE_SCRIPT_LIB set from the root");
        expectTrue(hasEntry(env, "PYTHONPATH=/opt/resolve/Developer/Scripting/Modules"),
                   "PYTHONPATH names the Modules dir");
        expectTrue(hasEntry(env, "HOME=/home/matt") && hasEntry(env, "DISPLAY=:0"),
                   "the rest of the parent environment passes through");
    }
    // Resolve's own values win over stale ones inherited from the parent
    // (another install's), each key exactly once.
    {
        const std::vector<std::string> env = pythonHelperEnvironment(
            {"RESOLVE_SCRIPT_LIB=/old/fusionscript.so", "RESOLVE_SCRIPT_API=/old/Scripting"},
            "/opt/resolve");
        expectTrue(hasEntry(env, "RESOLVE_SCRIPT_LIB=/opt/resolve/libs/Fusion/fusionscript.so") &&
                       countKey(env, "RESOLVE_SCRIPT_LIB") == 1,
                   "an inherited RESOLVE_SCRIPT_LIB is replaced, not duplicated");
        expectTrue(countKey(env, "RESOLVE_SCRIPT_API") == 1, "an inherited RESOLVE_SCRIPT_API is replaced");
    }
    // An inherited PYTHONPATH is kept, behind the Modules dir.
    {
        const std::vector<std::string> env =
            pythonHelperEnvironment({"PYTHONPATH=/home/matt/lib"}, "/opt/resolve");
        expectTrue(hasEntry(env, "PYTHONPATH=/opt/resolve/Developer/Scripting/Modules:/home/matt/lib") &&
                       countKey(env, "PYTHONPATH") == 1,
                   "an inherited PYTHONPATH follows the Modules dir");
    }
    // PYTHONHOME would point /usr/bin/python3 at another interpreter's
    // stdlib (a host that bundles its own Python); the system python must
    // find its own.
    {
        const std::vector<std::string> env =
            pythonHelperEnvironment({"PYTHONHOME=/opt/resolve/libs/python", "A=1"}, "/opt/resolve");
        expectTrue(countKey(env, "PYTHONHOME") == 0 && hasEntry(env, "A=1"),
                   "PYTHONHOME is dropped for the system python");
    }
}

static void testDescriptorsToClose()
{
    using ndi_timelinewatch_linux::descriptorsToClose;

    // The spawn's fallback when glibc has no addclosefrom_np (< 2.34, e.g.
    // the Rocky 8 baseline the plugin is built against): every descriptor
    // listed in /proc/self/fd from 3 up gets a close action, so Resolve's
    // inheritable sockets never reach the helper (LEARNINGS 2026-10-29).
    // readdir also hands back "." and "..", which are not descriptors.
    const std::vector<int> fds =
        descriptorsToClose({".", "..", "0", "1", "2", "3", "9", "49", "1023"});
    expectTrue(fds == std::vector<int>({3, 9, 49, 1023}), "fds 3 and up are closed, stdio kept");
    expectTrue(descriptorsToClose({"0", "1", "2"}).empty(), "nothing above stdio, nothing to close");
    expectTrue(descriptorsToClose({"7x", "-4", "", "99999999999999999999"}).empty(),
               "names that aren't a descriptor number are skipped");
}

int main()
{
    testDescriptorsToClose();
    testResolveRootForHostExecutable();
    testPathsBesideTheRoot();
    testBundleResourcePath();
    testPythonHelperEnvironment();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all linux timeline-watch tests passed\n");
    return 0;
}
