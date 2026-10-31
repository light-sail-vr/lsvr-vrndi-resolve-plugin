// Runtime-loaded NDI library on Linux (docs/linux-port-spec.md decision 8,
// amended by #71). libndi.so.6 ships inside the bundle, beside
// NDIOutput.ofx, but the plugin does NOT link it: libndi itself has
// NEEDED libavahi-client.so.3 / libavahi-common.so.3, so a link-time import
// would make the whole .ofx fail to dlopen — and vanish from Resolve's
// Effects Library without a word — on any machine without the Avahi client
// libraries. Instead the plugin dlopens the bundled copy by full path (found
// via dladdr on its own code) on the first NDI use and calls every NDI
// function through the table NDIlib_v5_load() returns (the SDK's
// Processing.NDI.DynamicLoad.h pattern). A missing runtime or missing Avahi
// is then a pass-through plugin with a logged reason and a Stream Status
// message.
//
// Unlike Windows (NDIRuntimeLoader.h, a preload ahead of a delay-loaded
// import), there is no import at all, so nothing else can bind the library
// behind our back.
//
// The path derivation and the result handling are pure and platform-neutral
// so the unit suite runs them everywhere (tests/test_linux_ndi_runtime.cpp);
// the dladdr/dlopen binding lives in src/LinuxNDIRuntime.cpp, compiled into
// the plugin on Linux only. NDI_RUNTIME_DLOPEN (defined by CMake for the
// Linux plugin target) gates the plugin's call sites.

#ifndef LINUX_NDI_RUNTIME_H
#define LINUX_NDI_RUNTIME_H

#include <atomic>
#include <string>

namespace ndi_linux_runtime {

// Why NDI is off for the rest of the process (a failed runtime load or
// NDIlib_initialize can't succeed later), for Stream Status. Latches exactly
// once: the first latch() claims it, writes the reason and publishes off()
// (release); every later latch() is a no-op. So once a reader sees off()
// (acquire) the reason is immutable and render threads may read it while
// another thread is calling latch() again. latch() needs no caller lock.
class OffLatch {
public:
    // false (reason untouched) when already latched.
    bool latch(const std::string& why)
    {
        if (claimed_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        reason_ = why;
        off_.store(true, std::memory_order_release);
        return true;
    }

    bool off() const { return off_.load(std::memory_order_acquire); }

    // Only after off() returned true.
    const std::string& reason() const { return reason_; }

private:
    std::atomic<bool> claimed_{false};
    std::atomic<bool> off_{false};
    std::string reason_;
};

// Path of `libName` in the directory containing `modulePath`. Empty when
// modulePath has no directory component. '/' is the only separator.
inline std::string siblingLibraryPath(const std::string& modulePath, const std::string& libName)
{
    const size_t slash = modulePath.rfind('/');
    if (slash == std::string::npos) {
        return std::string();
    }
    return modulePath.substr(0, slash + 1) + libName;
}

// The dynamic loader, injected: src/LinuxNDIRuntime.cpp binds these to
// dladdr/dlopen/dlsym/dlerror; the unit test binds fakes.
struct LoaderOps {
    std::string (*modulePath)();                      // the plugin's own file ("" if unknown)
    void* (*open)(const char* pathOrName);            // nullptr on failure, see lastError
    void* (*symbol)(void* handle, const char* name);  // nullptr on failure, see lastError
    std::string (*lastError)();
};

enum class LoadStatus {
    Loaded,
    NotFound,      // neither the bundle copy nor a system one opened
    NoEntryPoint,  // opened, but it has no `entryPoint` (not an NDI 5+ runtime)
    NullTable,     // the entry point returned null (the CI stub does exactly this)
};

struct LoadResult {
    LoadStatus status = LoadStatus::Loaded;
    bool fromBundle = false;      // true: opened beside the plugin
    std::string path;             // what was opened (bundle path, or the bare name)
    std::string bundleError;      // why the bundle attempt failed (dlerror, or skipped)
    std::string systemError;      // dlerror of the by-name fallback
    std::string symbolError;      // dlerror of the entry-point lookup
    const void* table = nullptr;  // what the entry point returned (an NDIlib_v5*)
};

// Opens `libName` beside the plugin, falling back to the system search by
// bare name (a system-wide NDI runtime, as on Windows), then resolves
// `entryPoint` (a function taking nothing and returning the API table) and
// calls it. Never throws. The handle is never closed: the runtime stays
// loaded for the process lifetime, like NDIlib_initialize.
inline LoadResult loadRuntime(const LoaderOps& ops, const char* libName, const char* entryPoint)
{
    LoadResult result;
    void* handle = nullptr;
    const std::string bundlePath = siblingLibraryPath(ops.modulePath(), libName);
    if (bundlePath.empty()) {
        result.bundleError = "plugin location unknown (dladdr failed)";
    } else {
        handle = ops.open(bundlePath.c_str());
        if (handle) {
            result.fromBundle = true;
            result.path = bundlePath;
        } else {
            result.bundleError = ops.lastError();
        }
    }
    if (!handle) {
        result.path = libName;
        handle = ops.open(libName);
        if (!handle) {
            result.systemError = ops.lastError();
            result.status = LoadStatus::NotFound;
            return result;
        }
    }

    using EntryPoint = const void* (*)();
    EntryPoint load = reinterpret_cast<EntryPoint>(ops.symbol(handle, entryPoint));
    if (!load) {
        result.symbolError = ops.lastError();
        result.status = LoadStatus::NoEntryPoint;
        return result;
    }
    result.table = load();
    if (!result.table) {
        result.status = LoadStatus::NullTable;
    }
    return result;
}

// loadRuntime bound to dladdr/dlopen/dlsym (src/LinuxNDIRuntime.cpp, Linux
// plugin builds only).
LoadResult loadBundledRuntime(const char* libName, const char* entryPoint);

// One line for the Stream Status parameter; empty when the runtime loaded.
// The Avahi case gets its own wording because it is the likely one on a
// fresh machine (libndi links libavahi-client.so.3) and the fix is a package.
inline std::string statusText(const LoadResult& r)
{
    switch (r.status) {
        case LoadStatus::Loaded:
            return std::string();
        case LoadStatus::NotFound:
            if (r.bundleError.find("libavahi") != std::string::npos ||
                r.systemError.find("libavahi") != std::string::npos) {
                return "NDI off — Avahi client library missing (install libavahi-client3 / avahi-libs)";
            }
            return "NDI off — libndi.so.6 missing from the plugin bundle";
        case LoadStatus::NoEntryPoint:
            return "NDI off — libndi.so.6 is not an NDI 5+ runtime";
        case LoadStatus::NullTable:
            return "NDI off — libndi.so.6 returned no API (CI stub build?)";
    }
    return std::string();
}

// The full story for the log: every path tried and every dlerror.
inline std::string logText(const LoadResult& r)
{
    switch (r.status) {
        case LoadStatus::Loaded:
            if (r.fromBundle) {
                return "NDI runtime loaded from the bundle: " + r.path;
            }
            return "NDI runtime not beside the plugin (" + r.bundleError +
                   "); using the system one: " + r.path;
        case LoadStatus::NotFound:
            return "NDI runtime NOT LOADED - streaming disabled. Bundle: " + r.bundleError +
                   " | system search: " + r.systemError;
        case LoadStatus::NoEntryPoint:
            return "NDI runtime " + r.path + " has no NDIlib_v5_load (" + r.symbolError +
                   ") - streaming disabled";
        case LoadStatus::NullTable:
            return "NDI runtime " + r.path +
                   ": NDIlib_v5_load() returned NULL (stub library?) - streaming disabled";
    }
    return std::string();
}

} // namespace ndi_linux_runtime

#endif // LINUX_NDI_RUNTIME_H
