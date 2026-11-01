// Linux binding of the BRAW runtime seam (src/LinuxBRAWRuntime.h): the host
// exe from /proc/self/exe, the library from dlopen. Compiled into
// braw_reader on Linux only (CMakeLists.txt, NDI_HAS_BRAW_READER).

#include "LinuxBRAWRuntime.h"

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include <mutex>

namespace ndi_linux_braw {

namespace {

// Inside Resolve: /opt/resolve/bin/resolve. Readable by the process itself
// even though Resolve runs setgid (other users get EACCES), and an
// unreadable link still leaves the default-install fallback.
std::string exePath()
{
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        return std::string();
    }
    return std::string(buf, static_cast<size_t>(n));
}

// RTLD_LOCAL: if Resolve already has the library loaded this just takes a
// reference to that copy; otherwise none of its symbols become visible to
// anything else in the process.
void* openLibrary(const char* path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void* findSymbol(void* handle, const char* name)
{
    dlerror();  // a null symbol is only an error if dlerror says so
    return dlsym(handle, name);
}

std::string lastError()
{
    const char* e = dlerror();
    return e ? e : std::string();
}

} // namespace

const Api* runtime(std::string* error)
{
    static std::mutex mutex;
    static Api api;
    static bool bound = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (!bound) {
        const LoaderOps ops = {&exePath, &openLibrary, &findSymbol, &lastError};
        bound = load(ops, &api, error);
    }
    return bound ? &api : nullptr;
}

} // namespace ndi_linux_braw
