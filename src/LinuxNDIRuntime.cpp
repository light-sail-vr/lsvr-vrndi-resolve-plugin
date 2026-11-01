// Linux binding of the NDI runtime seam (src/LinuxNDIRuntime.h): the
// plugin's own path from dladdr, the library from dlopen. Compiled into the
// plugin on Linux only (CMakeLists.txt, NDI_RUNTIME_DLOPEN).

#include "LinuxNDIRuntime.h"

#include <dlfcn.h>

namespace ndi_linux_runtime {

namespace {

// The .ofx this code is linked into, as Resolve dlopened it.
std::string pluginPath()
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&pluginPath), &info) && info.dli_fname) {
        return info.dli_fname;
    }
    return std::string();
}

// RTLD_LOCAL: libndi's symbols (and its Avahi dependencies') never become
// visible to, or interpose on, anything else in Resolve's process.
void* openLibrary(const char* pathOrName)
{
    return dlopen(pathOrName, RTLD_NOW | RTLD_LOCAL);
}

void* findSymbol(void* handle, const char* name)
{
    return dlsym(handle, name);
}

std::string lastError()
{
    const char* e = dlerror();
    return e ? e : "unknown dynamic-loader error";
}

} // namespace

LoadResult loadBundledRuntime(const char* libName, const char* entryPoint)
{
    const LoaderOps ops = {&pluginPath, &openLibrary, &findSymbol, &lastError};
    return loadRuntime(ops, libName, entryPoint);
}

} // namespace ndi_linux_runtime
