// Tests for the Linux render-path seam (src/LinuxGpuPath.h, ticket #74):
// the fast-path gate, the outcome a render records, the Stream Status note
// that says why a frame took the CPU path, and the "NDI off" verdict when
// the plugin's own CUDA runtime can't copy Resolve's CUDA frames (the real
// runtime side runs natively on the box: tests/test_cuda_runtime_unavailable.cpp). Pure, so it runs on every
// platform; the fallbacks themselves are exercised on the box (Tier 2:
// Resolve set to OpenCL, and a forced CUDA init failure).
// Build & run: ctest (Windows, Linux); on macOS by hand:
//   c++ -std=c++17 -Isrc tests/test_linux_gpu_path.cpp -o build/t && build/t

#include "LinuxGpuPath.h"

#include <cstdio>
#include <set>
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
using ndi_gpu_path::Outcome;
using ndi_gpu_path::Path;
using ndi_gpu_path::fastPathGate;
using ndi_gpu_path::pathOf;
using ndi_gpu_path::statusNote;

static const Outcome kAllOutcomes[] = {
    Outcome::GpuNative,        Outcome::HostCpuFrames,     Outcome::GpuParamOff,
    Outcome::OptimalFormatOff, Outcome::ModuleNotTried,    Outcome::ModuleNoDevice,
    Outcome::ModuleInitFailed, Outcome::ModuleForced,      Outcome::ColorUploadFailed,
    Outcome::StmapUploadFailed, Outcome::SubmitInvalid,
};

static void testGateGpuNative()
{
    const Outcome o = fastPathGate(true, true, true, Module::Ready);
    expectTrue(o == Outcome::GpuNative, "param on + optimal format + live context -> try GPU-native");
    expectTrue(pathOf(o) == Path::GpuNative, "GPU-native outcome is the GPU-native path");
    expectTrue(std::string(statusNote(o)).empty(), "GPU-native adds no Stream Status note");
}

static void testGateParamsOff()
{
    Outcome o = fastPathGate(false, true, true, Module::Ready);
    expectTrue(o == Outcome::GpuParamOff && contains(statusNote(o), "GPU Acceleration"),
               "GPU Acceleration off -> readback, names the param");
    o = fastPathGate(true, false, true, Module::Ready);
    expectTrue(o == Outcome::OptimalFormatOff && contains(statusNote(o), "Optimal Color Format"),
               "Optimal Color Format off -> readback, names the param");
    // The param read at the top of the render wins over a module that then
    // failed (the plugin clears its working copy of the param on failure).
    o = fastPathGate(false, true, false, Module::InitFailed);
    expectTrue(o == Outcome::GpuParamOff, "param off is reported as the param, not the module");
}

static void testGateNoContext()
{
    struct Case {
        Module module;
        Outcome expected;
        const char* needle;
        const char* name;
    } cases[] = {
        {Module::NoDevice, Outcome::ModuleNoDevice, "no CUDA device", "no device -> readback, says no device"},
        {Module::InitFailed, Outcome::ModuleInitFailed, "init failed", "init failure -> readback, says init failed"},
        {Module::ForcedFailure, Outcome::ModuleForced, "NDI_OUTPUT_FORCE_CUDA_FAIL",
         "forced failure -> readback, names the switch"},
        {Module::NotTried, Outcome::ModuleNotTried, "not initialized", "untried module -> readback"},
        // Shut down (or torn down) under a render while the module said Ready:
        // the old decide() called this GPU-native; the gate follows the context.
        {Module::Ready, Outcome::ModuleNotTried, "not initialized",
         "Ready module without a live context -> readback, not GPU-native"},
    };
    for (const Case& c : cases) {
        const Outcome o = fastPathGate(true, true, false, c.module);
        expectTrue(o == c.expected && pathOf(o) == Path::CpuReadback && contains(statusNote(o), "CPU path") &&
                       contains(statusNote(o), c.needle),
                   c.name);
    }
}

static void testFastPathFailuresAreReported()
{
    // renderGPUFrame passed the gate, then the colour upload, the STMap
    // upload or the submit failed: the frame is read back, and Stream
    // Status must say so (the old decide() still said GPU-native).
    struct Case {
        Outcome outcome;
        const char* needle;
        const char* name;
    } cases[] = {
        {Outcome::ColorUploadFailed, "Output Color", "colour upload failure -> readback, names Output Color"},
        {Outcome::StmapUploadFailed, "STMap", "STMap upload failure -> readback, names the STMap"},
        {Outcome::SubmitInvalid, "refused", "kernel refusal (INVALID) -> readback, says refused"},
    };
    for (const Case& c : cases) {
        expectTrue(pathOf(c.outcome) == Path::CpuReadback && contains(statusNote(c.outcome), "CPU path") &&
                       contains(statusNote(c.outcome), c.needle),
                   c.name);
    }
}

static void testHostCpuFrames()
{
    const Outcome o = Outcome::HostCpuFrames;
    expectTrue(pathOf(o) == Path::CpuHostFrames, "CPU frames from Resolve -> CPU path");
    expectTrue(contains(statusNote(o), "CPU path") && contains(statusNote(o), "CUDA"),
               "CPU frames note names the CPU path and points at Resolve's GPU mode");
}

static void testEveryFallbackHasItsOwnNote()
{
    std::set<std::string> notes;
    for (Outcome o : kAllOutcomes) {
        if (o == Outcome::GpuNative) {
            continue;
        }
        expectTrue(pathOf(o) != Path::GpuNative, "a fallback outcome is never the GPU-native path");
        expectTrue(contains(statusNote(o), "CPU path: "), "a fallback note starts with \"CPU path: \"");
        notes.insert(statusNote(o));
    }
    expectTrue(notes.size() == sizeof(kAllOutcomes) / sizeof(kAllOutcomes[0]) - 1,
               "every fallback outcome has a distinct note");
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

static void testCudaRuntimeUnavailable()
{
    using ndi_gpu_path::CopyFailure;
    using ndi_gpu_path::classifyCopyFailure;
    expectTrue(classifyCopyFailure(nullptr) == CopyFailure::Transient,
               "failed copy, runtime reaches a device -> transient (retry next frame, no latch)");
    expectTrue(classifyCopyFailure("") == CopyFailure::Transient, "empty probe error -> transient");
    const char* err = "CUDA driver version is insufficient for CUDA runtime version";
    expectTrue(classifyCopyFailure(err) == CopyFailure::RuntimeUnusable,
               "failed copy + runtime error -> runtime unusable (latch)");
    expectTrue(classifyCopyFailure("no CUDA device") == CopyFailure::RuntimeUnusable,
               "failed copy + no device for our runtime -> runtime unusable");

    const std::string status = ndi_gpu_path::cudaOffStatus(err);
    expectTrue(status.rfind("NDI off", 0) == 0, "CUDA-off status says NDI is off (not \"CPU path\")");
    expectTrue(!contains(status.c_str(), "CPU path"), "CUDA-off status never claims the CPU path");
    expectTrue(contains(status.c_str(), err), "CUDA-off status carries the runtime's reason");
    expectTrue(contains(status.c_str(), "OpenCL") && contains(status.c_str(), "driver"),
               "CUDA-off status names both fixes (driver, or OpenCL)");
}

int main()
{
    testGateGpuNative();
    testGateParamsOff();
    testGateNoContext();
    testFastPathFailuresAreReported();
    testHostCpuFrames();
    testEveryFallbackHasItsOwnNote();
    testForcedFailureSwitch();
    testCudaRuntimeUnavailable();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("All render-path tests passed\n");
    return 0;
}
