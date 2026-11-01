// Runtime-loaded Blackmagic RAW API on Linux (docs/linux-port-spec.md
// decision 19, ticket #77). The Camera Metadata projection reads the clip's
// immersive calibration through the host's own libBlackmagicRawAPI.so —
// inside Resolve that is /opt/resolve/libs/libBlackmagicRawAPI.so, the copy
// Resolve itself decodes BRAW with — so the plugin bundles nothing
// Blackmagic and cannot skew versions against the host. The directory is
// derived from the host exe (/proc/self/exe = /opt/resolve/bin/resolve →
// /opt/resolve/libs), with the default Resolve install as the fallback for
// standalone processes such as test_braw_reader.
//
// This is the plugin's own minimal dlopen dispatch, standing in for the
// SDK's BlackmagicRawAPIDispatch.cpp (third_party/braw/Linux/README.md
// explains why): it binds the three entry points the reader calls and
// reports, per path tried, why a load failed. Every failure is a false
// return with a reason — never a throw, never a half-bound Api.
//
// The path derivation and the loader's result handling are pure and
// platform-neutral so the unit suite runs them everywhere
// (tests/test_linux_braw_runtime.cpp); the readlink/dlopen binding lives in
// src/LinuxBRAWRuntime.cpp, compiled into braw_reader on Linux only.

#ifndef LINUX_BRAW_RUNTIME_H
#define LINUX_BRAW_RUNTIME_H

#include <string>
#include <vector>

namespace ndi_linux_braw {

static const char* const kDefaultResolveLibs = "/opt/resolve/libs";
static const char* const kLibraryName = "libBlackmagicRawAPI.so";

// Directories to look for kLibraryName in, in order: the host's ../libs
// (Resolve's layout), then the default Resolve install. '/' is the only
// separator; an exe path without two components contributes nothing.
inline std::vector<std::string> runtimeDirectories(const std::string& exePath)
{
    std::vector<std::string> dirs;
    const size_t exeSlash = exePath.rfind('/');
    if (exeSlash != std::string::npos && exeSlash > 0) {
        const size_t binSlash = exePath.rfind('/', exeSlash - 1);
        if (binSlash != std::string::npos) {
            dirs.push_back(exePath.substr(0, binSlash + 1) + "libs");
        }
    }
    if (dirs.empty() || dirs[0] != kDefaultResolveLibs) {
        dirs.push_back(kDefaultResolveLibs);
    }
    return dirs;
}

// The dynamic loader, injected: src/LinuxBRAWRuntime.cpp binds these to
// readlink(/proc/self/exe)/dlopen/dlsym/dlerror; the unit test binds fakes.
struct LoaderOps {
    std::string (*exePath)();                         // host exe ("" if unknown)
    void* (*open)(const char* path);                  // nullptr on failure, see lastError
    void* (*symbol)(void* handle, const char* name);  // nullptr on failure, see lastError
    std::string (*lastError)();
};

// The bound runtime. Pointers stay untyped here so this header (and its
// test) never needs the BRAW SDK header.
struct Api {
    void* createFactory = nullptr;  // CreateBlackmagicRawFactoryInstance
    void* variantInit = nullptr;    // VariantInit
    void* variantClear = nullptr;   // VariantClear
    std::string libraryPath;
};

// Binds the first candidate that opens AND exports every entry point. On
// failure returns false, leaves *out untouched and sets *error to the
// loader's reason for each path tried. A rejected library stays open (never
// dlclose inside Resolve's process).
inline bool load(const LoaderOps& ops, Api* out, std::string* error)
{
    std::string tried;
    for (const std::string& dir : runtimeDirectories(ops.exePath())) {
        const std::string path = dir + "/" + kLibraryName;
        void* handle = ops.open(path.c_str());
        if (handle == nullptr) {
            tried += (tried.empty() ? "" : "; ") + ops.lastError();
            continue;
        }
        Api api;
        std::string missing;
        const auto bind = [&](const char* name) {
            void* fn = ops.symbol(handle, name);
            if (fn == nullptr && missing.empty()) {
                missing = ops.lastError();
                if (missing.empty()) missing = path + ": no " + name;
            }
            return fn;
        };
        api.createFactory = bind("CreateBlackmagicRawFactoryInstance");
        api.variantInit = bind("VariantInit");
        api.variantClear = bind("VariantClear");
        if (!missing.empty()) {
            tried += (tried.empty() ? "" : "; ") + missing;
            continue;
        }
        api.libraryPath = path;
        *out = api;
        return true;
    }
    if (error) *error = std::string(kLibraryName) + " not loadable (" + tried + ")";
    return false;
}

// The process-wide runtime: binds on the first call that succeeds and stays
// bound; a failed bind is retried on the next call (the reader only runs on
// parameter changes, so the retry is cheap). nullptr + *error on failure.
// Defined in src/LinuxBRAWRuntime.cpp (Linux only). Thread-safe.
const Api* runtime(std::string* error);

} // namespace ndi_linux_braw

#endif
