// Which path a Linux render takes, and why (ticket #74; linux-port-spec
// user story 5, decisions 12 and 15). The plugin declares CUDA render +
// CUDA-stream support; Resolve then hands CUDA device buffers when its GPU
// processing mode is CUDA and CPU buffers otherwise (OpenCL). Device buffers
// go through the fused CUDA kernels when the module is up; otherwise they
// are read back through the host's stream and take the CPU path, which a
// working host stream makes possible even when our own module failed.
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
    NotTried,       // no init attempted yet
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

struct Inputs {
    bool hostCudaFrames = false;  // kOfxImageEffectPropCudaEnabled on this render
    bool gpuAcceleration = true;  // the GPU Acceleration param
    bool optimalFormat = true;    // the Optimal Color Format param (UYVY)
    Module module = Module::NotTried;
};

struct Decision {
    Path path;
    const char* status;  // Stream Status note; "" on the GPU-native path
};

inline Decision decide(const Inputs& in)
{
    if (!in.hostCudaFrames) {
        return {Path::CpuHostFrames, "CPU path: Resolve sends CPU frames (set GPU processing to CUDA)"};
    }
    if (!in.gpuAcceleration) {
        return {Path::CpuReadback, "CPU path: GPU Acceleration is off"};
    }
    if (!in.optimalFormat) {
        return {Path::CpuReadback, "CPU path: Optimal Color Format is off"};
    }
    switch (in.module) {
        case Module::Ready:
            return {Path::GpuNative, ""};
        case Module::NoDevice:
            return {Path::CpuReadback, "CPU path: no CUDA device for the plugin (driver too old?)"};
        case Module::InitFailed:
            return {Path::CpuReadback, "CPU path: CUDA init failed (see log)"};
        case Module::ForcedFailure:
            return {Path::CpuReadback, "CPU path: CUDA init failure forced (NDI_OUTPUT_FORCE_CUDA_FAIL)"};
        case Module::NotTried:
        default:
            return {Path::CpuReadback, "CPU path: CUDA not initialized"};
    }
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
