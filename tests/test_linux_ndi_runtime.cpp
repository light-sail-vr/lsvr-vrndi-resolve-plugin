// Tests for the Linux NDI runtime seam (src/LinuxNDIRuntime.h, spec
// decision 8): the sibling-library path derivation and the loader's
// result handling are pure, so they run on every platform; the dladdr/
// dlopen binding (src/LinuxNDIRuntime.cpp) is exercised on the box (Tier 2)
// and by the failure test in BUILD.md.
// Build & run: ctest (Windows, Linux); on macOS by hand:
//   c++ -std=c++17 -Isrc tests/test_linux_ndi_runtime.cpp -o build/t && build/t

#include "LinuxNDIRuntime.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
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

static void testSiblingLibraryPath()
{
    using ndi_linux_runtime::siblingLibraryPath;

    // The installed bundle layout (spec decision 7).
    expectTrue(siblingLibraryPath(
                   "/usr/OFX/Plugins/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx",
                   "libndi.so.6") ==
                   "/usr/OFX/Plugins/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6",
               "bundle layout resolves beside the module");

    // A file directly under the root keeps the root.
    expectTrue(siblingLibraryPath("/NDIOutput.ofx", "libndi.so.6") == "/libndi.so.6",
               "module at / resolves to /libndi.so.6");

    // dladdr reports the path the module was opened by, which may be
    // relative (a test harness dlopening "./x.ofx").
    expectTrue(siblingLibraryPath("./stage/NDIOutput.ofx", "libndi.so.6") ==
                   "./stage/libndi.so.6",
               "relative module path keeps its directory");

    // No directory component: nothing to resolve against.
    expectTrue(siblingLibraryPath("NDIOutput.ofx", "libndi.so.6").empty(),
               "bare filename yields empty");
    expectTrue(siblingLibraryPath("", "libndi.so.6").empty(), "empty module path yields empty");

    // A backslash is an ordinary filename byte on Linux, never a separator.
    expectTrue(siblingLibraryPath("/opt/a\\b/NDIOutput.ofx", "libndi.so.6") ==
                   "/opt/a\\b/libndi.so.6",
               "backslash is not a separator");

    // Non-ASCII (UTF-8) directories survive byte for byte.
    expectTrue(siblingLibraryPath("/home/m/st\xC3\xA9r\xC3\xA9o/NDIOutput.ofx", "libndi.so.6") ==
                   "/home/m/st\xC3\xA9r\xC3\xA9o/libndi.so.6",
               "UTF-8 path preserved");
}

// A fake dynamic loader: which paths open, what dlerror says for the ones
// that don't, and what the entry point returns. Plain function pointers (the
// seam's shape), so the state is file-static and reset per case.
namespace fake {
static std::string module;            // what dladdr reports
static std::string opensOk;           // the one path that opens
static std::string openError;         // dlerror for every other path
static bool hasEntryPoint = true;
static const void* table = nullptr;   // what NDIlib_v5_load() returns
static std::string lastError;
static std::string openedPaths;       // "a|b|" in call order
static int handleToken = 0;

static std::string modulePath() { return module; }
static void* open(const char* path)
{
    openedPaths += std::string(path) + "|";
    if (path == opensOk) {
        return &handleToken;
    }
    lastError = openError;
    return nullptr;
}
static const void* entryPoint() { return table; }
static void* symbol(void*, const char* name)
{
    if (hasEntryPoint && std::string(name) == "NDIlib_v5_load") {
        return reinterpret_cast<void*>(&entryPoint);
    }
    lastError = std::string("undefined symbol: ") + name;
    return nullptr;
}
static std::string error() { return lastError; }

static void reset()
{
    module = "/usr/OFX/Plugins/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx";
    opensOk.clear();
    openError = "cannot open shared object file: No such file or directory";
    hasEntryPoint = true;
    table = nullptr;
    lastError.clear();
    openedPaths.clear();
}

static const ndi_linux_runtime::LoaderOps ops = {&modulePath, &open, &symbol, &error};
} // namespace fake

static const char* kBundled = "/usr/OFX/Plugins/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6";

static ndi_linux_runtime::LoadResult loadFake()
{
    return ndi_linux_runtime::loadRuntime(fake::ops, "libndi.so.6", "NDIlib_v5_load");
}

static void testLoadFromBundle()
{
    using ndi_linux_runtime::LoadStatus;
    static const int someTable = 42;

    fake::reset();
    fake::opensOk = kBundled;
    fake::table = &someTable;
    const ndi_linux_runtime::LoadResult r = loadFake();
    expectTrue(r.status == LoadStatus::Loaded, "bundled runtime loads");
    expectTrue(r.fromBundle, "reported as loaded from the bundle");
    expectTrue(r.table == &someTable, "table is what the entry point returned");
    expectTrue(r.path == kBundled, "path is the bundled library");
    expectTrue(fake::openedPaths == std::string(kBundled) + "|",
               "only the bundled path was opened");
}

static void testSystemFallback()
{
    using ndi_linux_runtime::LoadStatus;
    static const int someTable = 7;

    // Bundled copy missing, a system-wide NDI runtime on the loader path.
    fake::reset();
    fake::opensOk = "libndi.so.6";
    fake::table = &someTable;
    const ndi_linux_runtime::LoadResult r = loadFake();
    expectTrue(r.status == LoadStatus::Loaded, "system runtime loads when the bundle has none");
    expectTrue(!r.fromBundle, "reported as the system fallback");
    expectTrue(r.path == "libndi.so.6", "fallback opens by bare name");
    expectTrue(r.bundleError == fake::openError, "bundle attempt's dlerror kept for the log");
    expectTrue(fake::openedPaths == std::string(kBundled) + "|libndi.so.6|",
               "bundle tried first, then the system search");
}

static void testNotFound()
{
    using ndi_linux_runtime::LoadStatus;

    // The Avahi case: the bundled libndi exists but its own NEEDED
    // libavahi-client.so.3 doesn't, and there is no system runtime either.
    fake::reset();
    fake::openError = "libavahi-common.so.3: cannot open shared object file: No such file or directory";
    const ndi_linux_runtime::LoadResult r = loadFake();
    expectTrue(r.status == LoadStatus::NotFound, "nothing opens -> NotFound");
    expectTrue(r.table == nullptr, "no table");
    expectTrue(r.bundleError == fake::openError, "bundle dlerror kept");
    expectTrue(r.systemError == fake::openError, "system dlerror kept");
}

static void testBadRuntime()
{
    using ndi_linux_runtime::LoadStatus;

    // A libndi.so.6 that isn't an NDI 5+ runtime (no NDIlib_v5_load).
    fake::reset();
    fake::opensOk = kBundled;
    fake::hasEntryPoint = false;
    ndi_linux_runtime::LoadResult r = loadFake();
    expectTrue(r.status == LoadStatus::NoEntryPoint, "missing entry point -> NoEntryPoint");
    expectTrue(r.symbolError == "undefined symbol: NDIlib_v5_load", "dlsym error kept");
    expectTrue(r.table == nullptr, "no table without an entry point");

    // The CI stub: every export, NDIlib_v5_load included, returns 0.
    fake::reset();
    fake::opensOk = kBundled;
    fake::table = nullptr;
    r = loadFake();
    expectTrue(r.status == LoadStatus::NullTable, "entry point returning null -> NullTable");
    expectTrue(r.fromBundle, "stub still reported as the bundle copy");
}

static void testUnknownModulePath()
{
    using ndi_linux_runtime::LoadStatus;
    static const int someTable = 1;

    // dladdr failed: no bundle attempt at all (never dlopen("")), straight to
    // the system search, with the reason recorded.
    fake::reset();
    fake::module.clear();
    fake::opensOk = "libndi.so.6";
    fake::table = &someTable;
    const ndi_linux_runtime::LoadResult r = loadFake();
    expectTrue(r.status == LoadStatus::Loaded, "system runtime still loads");
    expectTrue(fake::openedPaths == "libndi.so.6|", "no bundle attempt without a module path");
    expectTrue(r.bundleError == "plugin location unknown (dladdr failed)",
               "bundle error says why it was skipped");
}

static bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

static void testReasonText()
{
    using ndi_linux_runtime::LoadResult;
    using ndi_linux_runtime::LoadStatus;
    using ndi_linux_runtime::logText;
    using ndi_linux_runtime::statusText;

    LoadResult avahi;
    avahi.status = LoadStatus::NotFound;
    avahi.bundleError = "/b/libndi.so.6: libavahi-client.so.3: cannot open shared object file";
    avahi.systemError = "libndi.so.6: cannot open shared object file: No such file or directory";
    expectTrue(statusText(avahi) ==
                   "NDI off — Avahi client library missing (install libavahi-client3 / avahi-libs)",
               "missing Avahi named in Stream Status");
    expectTrue(contains(logText(avahi), avahi.bundleError) &&
                   contains(logText(avahi), avahi.systemError),
               "log carries both dlerror strings");

    LoadResult missing;
    missing.status = LoadStatus::NotFound;
    missing.bundleError = "/b/libndi.so.6: cannot open shared object file: No such file or directory";
    missing.systemError = "libndi.so.6: cannot open shared object file: No such file or directory";
    expectTrue(statusText(missing) == "NDI off — libndi.so.6 missing from the plugin bundle",
               "missing runtime named in Stream Status");

    LoadResult noEntry;
    noEntry.status = LoadStatus::NoEntryPoint;
    noEntry.path = "/b/libndi.so.6";
    noEntry.symbolError = "undefined symbol: NDIlib_v5_load";
    expectTrue(statusText(noEntry) == "NDI off — libndi.so.6 is not an NDI 5+ runtime",
               "foreign library named in Stream Status");
    expectTrue(contains(logText(noEntry), "/b/libndi.so.6") &&
                   contains(logText(noEntry), noEntry.symbolError),
               "log names the library and the dlsym error");

    LoadResult stub;
    stub.status = LoadStatus::NullTable;
    stub.path = "/b/libndi.so.6";
    expectTrue(statusText(stub) == "NDI off — libndi.so.6 returned no API (CI stub build?)",
               "stub runtime named in Stream Status");

    LoadResult ok;
    ok.status = LoadStatus::Loaded;
    ok.fromBundle = true;
    ok.path = "/b/libndi.so.6";
    expectTrue(statusText(ok).empty(), "a loaded runtime has no status message");
    expectTrue(logText(ok) == "NDI runtime loaded from the bundle: /b/libndi.so.6",
               "bundle load logged with its path");
}

// The NDI-off latch keeps its first reason: render threads read it after
// off() while instance create / param changes may latch again, so a second
// latch must never rewrite the string (ticket #73 review).
static void testOffLatch()
{
    ndi_linux_runtime::OffLatch latch;
    expectTrue(!latch.off(), "a fresh latch is not off");

    expectTrue(latch.latch("NDI off — first"), "the first latch claims it");
    expectTrue(latch.off() && latch.reason() == "NDI off — first",
               "off with the first reason");

    const char* before = latch.reason().c_str();
    expectTrue(!latch.latch("NDI off — second"), "a second latch is refused");
    expectTrue(latch.reason() == "NDI off — first" && latch.reason().c_str() == before,
               "a second latch leaves the reason string untouched");

    // Concurrent latchers: exactly one wins, and the reason is the winner's.
    ndi_linux_runtime::OffLatch raced;
    std::atomic<int> wins{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&raced, &wins, i]() {
            if (raced.latch("reason " + std::to_string(i))) {
                ++wins;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    expectTrue(wins == 1 && raced.off() && raced.reason().rfind("reason ", 0) == 0,
               "concurrent latches: exactly one wins");
}

int main()
{
    testOffLatch();
    testReasonText();
    testSiblingLibraryPath();
    testLoadFromBundle();
    testSystemFallback();
    testNotFound();
    testBadRuntime();
    testUnknownModulePath();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all Linux NDI runtime tests passed\n");
    return 0;
}
