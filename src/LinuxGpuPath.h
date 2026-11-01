// Which path a Linux render took, and why (ticket #74; linux-port-spec
// user story 5, decisions 12 and 15). The plugin declares CUDA render +
// CUDA-stream support; Resolve then hands CUDA device buffers when its GPU
// processing mode is CUDA and CPU buffers otherwise (OpenCL). Device buffers
// go through the fused CUDA kernels when the module is up; otherwise they
// are read back through the host's stream and take the CPU path, which a
// working host stream makes possible even when our own module failed.
//
// The render code records the Outcome at the point it commits to a path
// (renderGPUFrame, and render() for CPU frames), before that frame is
// submitted, so Stream Status describes the frame it was composed for. The
// fast-path gate below is the only copy of the "may we try the fused
// kernels" condition on Linux: the note can't drift from the path taken.
//
// Every CPU path gets a Stream Status note, because on Linux it caps what
// plays in real time (the CPU path managed ~15.5 fps at 1080p24 Full
// Resolution on the box, #73) and the user can usually fix the cause.
//
// Pure and platform-neutral so the unit suite runs it everywhere
// (tests/test_linux_gpu_path.cpp). NDI_GPU_PATH_STATUS (defined by CMake
// for the Linux CUDA plugin) gates the plugin's call sites.

#ifndef LINUX_GPU_PATH_H
#define LINUX_GPU_PATH_H

namespace ndi_gpu_path {

// The plugin's CUDA module for this instance.
enum class Module {
    NotTried,       // no init attempted yet, or shut down since
    Ready,
    NoDevice,       // cudaGetDeviceCount found no usable device or driver
    InitFailed,     // a device, but cuda_gpu_init failed
    ForcedFailure,  // NDI_OUTPUT_FORCE_CUDA_FAIL simulated an init failure
};

enum class Path {
    GpuNative,      // fused CUDA kernels, async slot ring
    CpuReadback,    // CUDA frames read back whole, then the CPU path
    CpuHostFrames,  // Resolve handed CPU frames: the CPU path
};

// What one render did. Everything but GpuNative and HostCpuFrames is a
// reason a CUDA frame was read back instead.
enum class Outcome {
    GpuNative,          // fused kernel enqueued (or the slot ring was full: frame dropped)
    HostCpuFrames,      // Resolve handed CPU buffers
    GpuParamOff,        // the GPU Acceleration param
    OptimalFormatOff,   // the Optimal Color Format param
    ModuleNotTried,     // no CUDA context (not initialized yet, or shut down)
    ModuleNoDevice,
    ModuleInitFailed,
    ModuleForced,
    ColorUploadFailed,  // the Output Color transform didn't reach the GPU
    StmapUploadFailed,  // the STMap didn't reach the GPU
    SubmitInvalid,      // the kernel refused this source (geometry, allocation)
};

inline Path pathOf(Outcome o)
{
    switch (o) {
        case Outcome::GpuNative:
            return Path::GpuNative;
        case Outcome::HostCpuFrames:
            return Path::CpuHostFrames;
        default:
            return Path::CpuReadback;
    }
}

// The Stream Status note; "" on the GPU-native path.
inline const char* statusNote(Outcome o)
{
    switch (o) {
        case Outcome::GpuNative:
            return "";
        case Outcome::HostCpuFrames:
            return "CPU path: Resolve sends CPU frames (set GPU processing to CUDA)";
        case Outcome::GpuParamOff:
            return "CPU path: GPU Acceleration is off";
        case Outcome::OptimalFormatOff:
            return "CPU path: Optimal Color Format is off";
        case Outcome::ModuleNoDevice:
            return "CPU path: no CUDA device for the plugin (driver too old?)";
        case Outcome::ModuleInitFailed:
            return "CPU path: CUDA init failed (see log)";
        case Outcome::ModuleForced:
            return "CPU path: CUDA init failure forced (NDI_OUTPUT_FORCE_CUDA_FAIL)";
        case Outcome::ColorUploadFailed:
            return "CPU path: Output Color upload to the GPU failed (see log)";
        case Outcome::StmapUploadFailed:
            return "CPU path: STMap upload to the GPU failed (see log)";
        case Outcome::SubmitInvalid:
            return "CPU path: the CUDA kernel refused this frame (see log)";
        case Outcome::ModuleNotTried:
        default:
            return "CPU path: CUDA not initialized";
    }
}

// Why there is no CUDA context. Ready without a context can only mean the
// context was torn down under us: report it as not initialized.
inline Outcome moduleOutcome(Module m)
{
    switch (m) {
        case Module::NoDevice:
            return Outcome::ModuleNoDevice;
        case Module::InitFailed:
            return Outcome::ModuleInitFailed;
        case Module::ForcedFailure:
            return Outcome::ModuleForced;
        case Module::NotTried:
        case Module::Ready:
        default:
            return Outcome::ModuleNotTried;
    }
}

// The fast-path gate for a CUDA frame: GpuNative when the fused kernels may
// be tried, otherwise why the frame is read back. gpuParam is the GPU
// Acceleration param as read at the top of the render (a failed module init
// clears the plugin's working copy mid-render; that's a module failure, not
// the user switching the param off). haveContext is a live CUDA context.
inline Outcome fastPathGate(bool gpuParam, bool optimalFormat, bool haveContext, Module module)
{
    if (!gpuParam) {
        return Outcome::GpuParamOff;
    }
    if (!optimalFormat) {
        return Outcome::OptimalFormatOff;
    }
    if (!haveContext) {
        return moduleOutcome(module);
    }
    return Outcome::GpuNative;
}

// NDI_OUTPUT_FORCE_CUDA_FAIL, read once at module init: set (to anything
// but "" or "0") it makes the plugin skip its CUDA init and behave as if
// the init had failed, so the fallback can be tested on a working box.
inline bool forcedFailure(const char* envValue)
{
    return envValue && envValue[0] != '\0' && !(envValue[0] == '0' && envValue[1] == '\0');
}

}  // namespace ndi_gpu_path

#endif  // LINUX_GPU_PATH_H
