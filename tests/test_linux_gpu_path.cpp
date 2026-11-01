// Tests for the Linux render-path seam (src/LinuxGpuPath.h, ticket #74):
// which path a render takes (GPU-native CUDA, or the CPU path) and the
// Stream Status note that says why. Pure, so it runs on every platform; the
// fallbacks themselves are exercised on the box (Tier 2: Resolve set to
// OpenCL, and a forced CUDA init failure).
// Build & run: ctest (Windows, Linux); on macOS by hand:
//   c++ -std=c++17 -Isrc tests/test_linux_gpu_path.cpp -o build/t && build/t

#include "LinuxGpuPath.h"

#include <cstdio>
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

static bool contains(const char* haystack, const char* needle)
{
    return std::string(haystack).find(needle) != std::string::npos;
}

using ndi_gpu_path::Module;
using ndi_gpu_path::Path;
using ndi_gpu_path::Inputs;
using ndi_gpu_path::decide;

static Inputs cudaFrames(Module module)
{
    Inputs in;
    in.hostCudaFrames = true;
    in.gpuAcceleration = true;
    in.optimalFormat = true;
    in.module = module;
    return in;
}

static void testGpuNative()
{
    const ndi_gpu_path::Decision d = decide(cudaFrames(Module::Ready));
    expectTrue(d.path == Path::GpuNative, "CUDA frames + ready module -> GPU-native");
    expectTrue(std::string(d.status).empty(), "GPU-native adds no Stream Status note");
}

static void testHostCpuFrames()
{
    // Resolve set to OpenCL hands CPU buffers, whatever our module did.
    Inputs in = cudaFrames(Module::Ready);
    in.hostCudaFrames = false;
    const ndi_gpu_path::Decision d = decide(in);
    expectTrue(d.path == Path::CpuHostFrames, "CPU frames from Resolve -> CPU path");
    expectTrue(contains(d.status, "CPU path"), "CPU frames note names the CPU path");
    expectTrue(contains(d.status, "CUDA"), "CPU frames note points at Resolve's GPU mode");

    in.module = Module::InitFailed;
    expectTrue(decide(in).path == Path::CpuHostFrames,
               "CPU frames win over a failed module (nothing to read back)");
}

static void testReadbackFallbacks()
{
    struct Case {
        Module module;
        const char* needle;
        const char* name;
    } cases[] = {
        {Module::NoDevice, "no CUDA device", "no device -> readback, says no device"},
        {Module::InitFailed, "init failed", "init failure -> readback, says init failed"},
        {Module::ForcedFailure, "NDI_OUTPUT_FORCE_CUDA_FAIL",
         "forced failure -> readback, names the switch"},
        {Module::NotTried, "CPU path", "untried module -> readback"},
    };
    for (const Case& c : cases) {
        const ndi_gpu_path::Decision d = decide(cudaFrames(c.module));
        expectTrue(d.path == Path::CpuReadback && contains(d.status, "CPU path") &&
                       contains(d.status, c.needle),
                   c.name);
    }
}

static void testParamsOff()
{
    Inputs in = cudaFrames(Module::Ready);
    in.gpuAcceleration = false;
    ndi_gpu_path::Decision d = decide(in);
    expectTrue(d.path == Path::CpuReadback && contains(d.status, "GPU Acceleration"),
               "GPU Acceleration off -> readback, names the param");

    in = cudaFrames(Module::Ready);
    in.optimalFormat = false;
    d = decide(in);
    expectTrue(d.path == Path::CpuReadback && contains(d.status, "Optimal Color Format"),
               "Optimal Color Format off -> readback, names the param");
}

static void testForcedFailureSwitch()
{
    using ndi_gpu_path::forcedFailure;
    expectTrue(!forcedFailure(nullptr), "unset env -> no forced failure");
    expectTrue(!forcedFailure(""), "empty env -> no forced failure");
    expectTrue(!forcedFailure("0"), "\"0\" -> no forced failure");
    expectTrue(forcedFailure("1"), "\"1\" -> forced failure");
    expectTrue(forcedFailure("yes"), "any other value -> forced failure");
}

int main()
{
    testGpuNative();
    testHostCpuFrames();
    testReadbackFallbacks();
    testParamsOff();
    testForcedFailureSwitch();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("All render-path tests passed\n");
    return 0;
}
