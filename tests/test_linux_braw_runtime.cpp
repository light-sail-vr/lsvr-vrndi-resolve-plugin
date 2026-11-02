// Tests for the Linux BRAW runtime seam (src/LinuxBRAWRuntime.h, spec
// decision 19, ticket #77): where the plugin looks for the host's
// libBlackmagicRawAPI.so, derived from the host exe path, and how the
// dispatch handles each way that lookup can fail. Pure, so it runs on every
// platform; the readlink/dlopen binding (src/LinuxBRAWRuntime.cpp) is
// exercised by test_braw_reader on the box.
// Build & run: ctest (Windows, Linux); on macOS by hand:
//   c++ -std=c++17 -Isrc tests/test_linux_braw_runtime.cpp -o build/t && build/t

#include "LinuxBRAWRuntime.h"

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

static void testResolveLayout()
{
    using ndi_linux_braw::runtimeDirectories;

    // Resolve installs as /opt/resolve/bin/resolve with its libraries in
    // /opt/resolve/libs (LEARNINGS.md, Linux environment map).
    const std::vector<std::string> dirs = runtimeDirectories("/opt/resolve/bin/resolve");
    expectTrue(!dirs.empty() && dirs[0] == "/opt/resolve/libs",
               "Resolve exe resolves to its libs directory first");
}

// Standalone processes (the unit-test binary) are not inside Resolve, so the
// exe-relative guess misses; the default Resolve install is the fallback.
static void testStandaloneFallback()
{
    using ndi_linux_braw::runtimeDirectories;

    const std::vector<std::string> test =
        runtimeDirectories("/home/matt/ndioutput-77/build/test_braw_reader");
    expectTrue(test.size() == 2 && test[0] == "/home/matt/ndioutput-77/libs" &&
                   test[1] == "/opt/resolve/libs",
               "standalone exe tries its own ../libs, then the default Resolve install");

    // Inside a default-location Resolve both rules name the same directory:
    // try it once.
    const std::vector<std::string> resolve = runtimeDirectories("/opt/resolve/bin/resolve");
    expectTrue(resolve.size() == 1, "default Resolve install is not tried twice");

    // No usable exe path (readlink failed): the fallback alone.
    const std::vector<std::string> unknown = runtimeDirectories("");
    expectTrue(unknown.size() == 1 && unknown[0] == "/opt/resolve/libs",
               "unknown exe path still tries the default Resolve install");
    const std::vector<std::string> bare = runtimeDirectories("resolve");
    expectTrue(bare.size() == 1 && bare[0] == "/opt/resolve/libs",
               "bare exe name still tries the default Resolve install");
}

// --- Dispatch failure handling: a fake dynamic loader ----------------------

namespace fake {

static std::string exe;
static std::vector<std::string> loadable;  // paths open() succeeds for
static std::vector<std::string> missing;   // "path:symbol" pairs dlsym misses
static std::vector<std::string> opened;    // every open() attempt, in order
static std::string error;
static int handles[8];

static std::string exePath() { return exe; }

static void* open(const char* path)
{
    opened.push_back(path);
    for (size_t i = 0; i < loadable.size(); ++i) {
        if (loadable[i] == path) return &handles[i];
    }
    error = std::string(path) + ": cannot open shared object file: No such file or directory";
    return nullptr;
}

static void* symbol(void* handle, const char* name)
{
    const size_t i = static_cast<int*>(handle) - handles;
    if (i >= loadable.size()) return nullptr;
    for (const std::string& m : missing) {
        if (m == loadable[i] + ":" + name) {
            error = loadable[i] + ": undefined symbol: " + name;
            return nullptr;
        }
    }
    // Any distinct non-null address identifies (library, symbol) for the test.
    return reinterpret_cast<char*>(handle) + std::string(name).size();
}

static std::string lastError() { return error; }

static void reset(const std::string& exePath, std::vector<std::string> canLoad)
{
    exe = exePath;
    loadable = std::move(canLoad);
    missing.clear();
    opened.clear();
    error.clear();
}

static const ndi_linux_braw::LoaderOps ops = {exePath, open, symbol, lastError};

} // namespace fake

static void testLoadsResolveCopy()
{
    fake::reset("/opt/resolve/bin/resolve", {"/opt/resolve/libs/libBlackmagicRawAPI.so"});
    ndi_linux_braw::Api api;
    std::string error;
    const bool ok = ndi_linux_braw::load(fake::ops, &api, &error);
    expectTrue(ok, "Resolve's own runtime loads");
    expectTrue(api.libraryPath == "/opt/resolve/libs/libBlackmagicRawAPI.so",
               "bound library is Resolve's libBlackmagicRawAPI.so");
    expectTrue(api.createFactory != nullptr && api.variantInit != nullptr &&
                   api.variantClear != nullptr,
               "factory and Variant entry points are bound");
    expectTrue(fake::opened.size() == 1, "nothing past the first hit is opened");
}

// No runtime anywhere (hosted CI; a machine without Resolve): false, every
// path tried named with its loader reason, and the caller's Api untouched.
static void testNotFound()
{
    fake::reset("/home/matt/build/test_braw_reader", {});
    ndi_linux_braw::Api api;
    api.libraryPath = "sentinel";
    std::string error;
    const bool ok = ndi_linux_braw::load(fake::ops, &api, &error);
    expectTrue(!ok, "no runtime returns false");
    expectTrue(api.libraryPath == "sentinel" && api.createFactory == nullptr,
               "failure leaves the Api untouched");
    expectTrue(fake::opened.size() == 2, "both candidates were tried");
    expectTrue(error.find("libBlackmagicRawAPI.so") != std::string::npos,
               "error names the library");
    expectTrue(error.find("/home/matt/libs/libBlackmagicRawAPI.so") != std::string::npos &&
                   error.find("/opt/resolve/libs/libBlackmagicRawAPI.so") != std::string::npos,
               "error names every path tried");
    expectTrue(error.find("No such file or directory") != std::string::npos,
               "error carries the loader's reason");
    std::printf("     (error: %s)\n", error.c_str());
}

// A library that opens but lacks an entry point (a stray, unrelated or
// pre-immersive libBlackmagicRawAPI.so) must not be bound: calling a null
// VariantClear would crash the host. The next candidate gets its turn.
static void testIncompleteRuntime()
{
    fake::reset("/home/matt/build/test_braw_reader",
                {"/home/matt/libs/libBlackmagicRawAPI.so",
                 "/opt/resolve/libs/libBlackmagicRawAPI.so"});
    fake::missing = {"/home/matt/libs/libBlackmagicRawAPI.so:CreateBlackmagicRawFactoryInstance"};
    ndi_linux_braw::Api api;
    std::string error;
    expectTrue(ndi_linux_braw::load(fake::ops, &api, &error) &&
                   api.libraryPath == "/opt/resolve/libs/libBlackmagicRawAPI.so",
               "library without the factory is skipped for the next candidate");

    fake::reset("/opt/resolve/bin/resolve", {"/opt/resolve/libs/libBlackmagicRawAPI.so"});
    fake::missing = {"/opt/resolve/libs/libBlackmagicRawAPI.so:VariantClear"};
    api = ndi_linux_braw::Api();
    error.clear();
    const bool ok = ndi_linux_braw::load(fake::ops, &api, &error);
    expectTrue(!ok && api.createFactory == nullptr,
               "library without VariantClear is not bound");
    expectTrue(error.find("undefined symbol: VariantClear") != std::string::npos,
               "error names the missing entry point");
    std::printf("     (error: %s)\n", error.c_str());
}

// runtime() retries a failed bind on every Camera Metadata read. A library
// that opened but was rejected for a missing entry point stays open (no
// dlclose inside Resolve), so a retry must not dlopen it again: each open
// would stack another reference. The rejection is remembered, with its
// reason, and only paths that never opened are retried.
static void testRetrySkipsRejectedLibrary()
{
    const std::string stray = "/home/matt/libs/libBlackmagicRawAPI.so";
    fake::reset("/home/matt/build/test_braw_reader", {stray});
    fake::missing = {stray + ":VariantInit"};
    ndi_linux_braw::Rejected rejected;
    ndi_linux_braw::Api api;

    std::string first;
    expectTrue(!ndi_linux_braw::load(fake::ops, &api, &first, &rejected),
               "incomplete library and absent fallback fail the first bind");
    expectTrue(fake::opened.size() == 2, "first bind tries both candidates");

    std::string second;
    fake::opened.clear();
    fake::error.clear();
    expectTrue(!ndi_linux_braw::load(fake::ops, &api, &second, &rejected),
               "retry still fails");
    expectTrue(fake::opened.size() == 1 && fake::opened[0] == "/opt/resolve/libs/libBlackmagicRawAPI.so",
               "retry does not reopen the rejected library");
    expectTrue(second.find(stray + ": undefined symbol: VariantInit") != std::string::npos,
               "retry error still names the rejected library's reason");
    std::printf("     (retry error: %s)\n", second.c_str());

    // Resolve's copy appears later (installed after the plugin first ran):
    // the retry binds it, still without touching the rejected library.
    fake::loadable.push_back("/opt/resolve/libs/libBlackmagicRawAPI.so");
    fake::opened.clear();
    std::string third;
    expectTrue(ndi_linux_braw::load(fake::ops, &api, &third, &rejected) &&
                   api.libraryPath == "/opt/resolve/libs/libBlackmagicRawAPI.so",
               "retry binds a runtime that became loadable");
    expectTrue(fake::opened.size() == 1, "...opening only that runtime");
}

int main()
{
    testResolveLayout();
    testStandaloneFallback();
    testLoadsResolveCopy();
    testNotFound();
    testIncompleteRuntime();
    testRetrySkipsRejectedLibrary();

    if (failures == 0) {
        std::printf("all linux-braw-runtime tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d linux-braw-runtime test(s) FAILED\n", failures);
    return 1;
}
