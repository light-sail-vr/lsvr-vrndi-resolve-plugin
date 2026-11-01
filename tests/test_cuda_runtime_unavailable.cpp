// The plugin's own CUDA runtime can't reach a GPU that exists (ticket #74
// review; src/LinuxGpuPath.h). In Resolve that happens when Resolve's CUDA
// works but the plugin's statically linked CUDA 12.9 runtime doesn't (a
// driver too old for it): Resolve still hands CUDA frames, and the plugin
// can neither copy nor read them back. This test simulates it for real on
// the box: CUDA_VISIBLE_DEVICES=-1, set before the first CUDA call, hides
// every device from this process's runtime while the GPU and driver stay
// fine. Then the module must
//   - name the reason (cuda_gpu_runtime_error),
//   - report no device and refuse to init (the plugin's Module::NoDevice),
//   - fail the passthrough copy and the readback, as for Resolve's buffers,
// and the plugin's verdict must be "runtime unusable" with an "NDI off"
// Stream Status carrying that reason, never "CPU path".
// Linux only (setenv). Passes in the GPU-less build container too, where
// the runtime finds no device for real.
// Run: ctest, or natively on the box (BUILD.md, "CUDA on Linux").

#include "CudaGPUAcceleration.h"
#include "LinuxGpuPath.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

int main()
{
    setenv("CUDA_VISIBLE_DEVICES", "-1", 1);  // before any CUDA call

    const char* runtimeError = cuda_gpu_runtime_error();
    std::printf("runtime probe: %s\n", runtimeError ? runtimeError : "(usable)");
    expectTrue(runtimeError != nullptr, "hidden devices -> the runtime probe names a reason");
    if (!runtimeError) {
        std::fprintf(stderr, "CUDA_VISIBLE_DEVICES=-1 did not hide the GPU; nothing else is meaningful\n");
        return 1;
    }
    expectTrue(!cuda_gpu_is_available(), "hidden devices -> cuda_gpu_is_available() is false");
    CudaGPUContextRef context = cuda_gpu_init();
    expectTrue(context == nullptr, "hidden devices -> cuda_gpu_init() fails");
    cuda_gpu_shutdown(context);

    // Resolve's frame and stream, as the plugin sees them: device pointers
    // it didn't allocate and the host's stream. The runtime fails before it
    // looks at either, so stand-ins are safe here: 0x1 is cudaStreamLegacy.
    void* hostStream = reinterpret_cast<void*>(0x1);
    void* src = reinterpret_cast<void*>(0x10000);
    void* dst = reinterpret_cast<void*>(0x20000);
    expectTrue(!cuda_gpu_copy_buffer(nullptr, hostStream, src, dst, 4096, false),
               "the passthrough copy of a CUDA frame fails");
    float cpu[1024];
    expectTrue(!cuda_gpu_read_buffer(nullptr, hostStream, src, cpu, sizeof(cpu)),
               "the readback of a CUDA frame fails too: no CPU path exists");

    const char* again = cuda_gpu_runtime_error();
    expectTrue(again != nullptr, "the probe still fails after the copy (the plugin latches it)");
    expectTrue(ndi_gpu_path::classifyCopyFailure(again) == ndi_gpu_path::CopyFailure::RuntimeUnusable,
               "verdict: runtime unusable");
    const std::string status = ndi_gpu_path::cudaOffStatus(again);
    std::printf("Stream Status: %s\n", status.c_str());
    expectTrue(status.rfind("NDI off", 0) == 0 && status.find(again) != std::string::npos,
               "Stream Status: NDI off, with the runtime's reason");

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("All CUDA-runtime-unavailable tests passed\n");
    return 0;
}
