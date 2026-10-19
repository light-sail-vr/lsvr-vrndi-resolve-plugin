// Correctness test for the GPU-native fast path (src/MetalGPUAcceleration.mm):
// the fused downscale+convert kernels must be byte-identical to the CPU
// reference composition (ndi_stream::downscaleRGBABox + the shared UYVY
// encoder, src/UYVYEncode.h), on a real Metal device. Also covers the passthrough copy, the
// full-frame readback used by the CPU fallback, and the STMap warp kernels
// (issue #7) against the ndi_stmap::warpRGBABox CPU reference.
// Build & run: make test-metal (macOS with any Metal device; skips cleanly without one)

#include "MetalGPUAcceleration.h"
#include "OutputColor.h"
#include "STMap.h"
#include "StreamResolution.h"
#include "UYVYEncode.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* name)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// Reference UYVY packing: the shared encoder the CPU path runs
// (src/UYVYEncode.h — vertical flip, BT.709 limited range), applied to an
// already-downscaled bottom-up float RGBA buffer. Kernels must match it
// byte-for-byte (issue #57).
static void referenceUYVY(const float* src, int width, int height, std::vector<uint8_t>& out)
{
    out.assign(static_cast<size_t>(width) * height * 2, 0);
    ndi_uyvy::convertRGBAToUYVYFlipped(src, width, height, out.data());
}

// Deterministic frame content: gradients across all channels, including
// out-of-range values so both paths must clamp identically.
static void fillFrame(std::vector<float>& frame, int width, int height, int rowFloats)
{
    frame.assign(static_cast<size_t>(height) * rowFloats, 999.0f); // poison padding
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float* px = frame.data() + static_cast<size_t>(y) * rowFloats + x * 4;
            px[0] = static_cast<float>(x) / (width - 1) * 1.4f - 0.2f; // sweeps past [0,1]
            px[1] = static_cast<float>(y) / (height - 1);
            px[2] = static_cast<float>((x * 7 + y * 13) % 17) / 16.0f;
            px[3] = 1.0f;
        }
    }
}

template <typename T>
static bool compareBuffers(const T* actual, const T* expected, size_t count, long tolerance,
                           const char* what)
{
    size_t bad = 0;
    long maxDiff = 0;
    for (size_t i = 0; i < count; ++i) {
        long diff = static_cast<long>(actual[i]) - static_cast<long>(expected[i]);
        if (diff < 0) diff = -diff;
        if (diff > maxDiff) maxDiff = diff;
        if (diff > tolerance) ++bad;
    }
    if (bad) {
        std::fprintf(stderr, "  %s: %zu/%zu values differ by more than %ld (max diff %ld)\n",
                     what, bad, count, tolerance, maxDiff);
    }
    return bad == 0;
}

int main()
{
    if (!metal_gpu_is_available()) {
        std::printf("Metal not available on this machine — skipping GPU kernel tests\n");
        return 0;
    }

    MetalGPUContextRef ctx = metal_gpu_init();
    if (!ctx) {
        std::fprintf(stderr, "FAIL metal_gpu_init\n");
        return 1;
    }

    const int srcW = 64, srcH = 32;

    struct Case { int divisor; int rowFloats; const char* name; };
    const Case cases[] = {
        {1, srcW * 4, "divisor 1, tight rows"},
        {2, srcW * 4, "divisor 2, tight rows"},
        {4, srcW * 4, "divisor 4, tight rows"},
        {2, (srcW + 2) * 4, "divisor 2, padded rows"},
    };

    for (const Case& c : cases) {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, c.rowFloats);

        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), frame.size() * sizeof(float));
        if (!srcBuf) {
            ++failures;
            std::fprintf(stderr, "FAIL create source buffer (%s)\n", c.name);
            continue;
        }

        int outW = 0, outH = 0;
        ndi_stream::outputDims(srcW, srcH, c.divisor, &outW, &outH);

        // CPU reference: box downscale (row order preserved), then flip+convert.
        std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
        ndi_stream::downscaleRGBABox(frame.data(), srcW, srcH, c.rowFloats, c.divisor,
                                     small.data(), outW, outH);

        {
            std::vector<uint8_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
            referenceUYVY(small.data(), outW, outH, expected);
            bool ran = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf,
                                                          srcW, srcH, c.rowFloats, c.divisor,
                                                          outW, outH, nullptr, actual.data());
            char name[128];
            std::snprintf(name, sizeof(name), "UYVY kernel matches CPU reference (%s)", c.name);
            check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 0, name), name);
        }

        metal_gpu_release_buffer(srcBuf);
    }

    // Passthrough copy: device-to-device blit must move bytes verbatim.
    // The test waits for completion before inspecting; the plugin passes
    // false and lets the host's queue ordering handle it.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        const size_t bytes = frame.size() * sizeof(float);
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), bytes);
        void* dstBuf = metal_gpu_create_shared_buffer(ctx, nullptr, bytes);
        bool ran = metal_gpu_copy_buffer(ctx, nullptr, srcBuf, dstBuf, bytes, true);
        check(ran && std::memcmp(metal_gpu_buffer_contents(dstBuf), frame.data(), bytes) == 0,
              "passthrough copy moves bytes verbatim");
        metal_gpu_release_buffer(srcBuf);
        metal_gpu_release_buffer(dstBuf);
    }

    // CPU-buffer renders with GPU Acceleration on upload the tight frame and
    // run the fused kernel at divisor 1 — same bytes as the CPU converter.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        std::vector<uint8_t> expected, actual(static_cast<size_t>(srcW) * srcH * 2, 0);
        referenceUYVY(frame.data(), srcW, srcH, expected);
        bool ran = metal_gpu_convert_rgba_to_uyvy(ctx, frame.data(), actual.data(), srcW, srcH, nullptr);
        check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 0,
                                    "CPU-buffer UYVY"),
              "CPU-buffer convert matches CPU reference byte-for-byte");
        std::vector<uint8_t> odd(static_cast<size_t>(srcW - 1) * srcH * 2, 0);
        check(!metal_gpu_convert_rgba_to_uyvy(ctx, frame.data(), odd.data(), srcW - 1, srcH, nullptr),
              "CPU-buffer convert refuses odd widths (CPU converter takes them)");
    }

    // Full-frame readback (the CPU-fallback ingest) returns the source bytes.
    {
        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        const size_t bytes = frame.size() * sizeof(float);
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), bytes);
        std::vector<float> readback(frame.size(), -1.0f);
        bool ran = metal_gpu_read_buffer(ctx, nullptr, srcBuf, readback.data(), bytes);
        check(ran && std::memcmp(readback.data(), frame.data(), bytes) == 0,
              "read_buffer returns source bytes");
        metal_gpu_release_buffer(srcBuf);
    }

    // Non-blocking submit path (v1.6.0): same kernels through the slot ring +
    // completion callback. Output must match the blocking path bit-for-bit,
    // the ring must refuse a submit when every slot is held, and released
    // slots must become claimable again.
    {
        struct AsyncCapture {
            std::mutex m;
            std::condition_variable cv;
            int fired = 0;
            bool ok = false;
            std::vector<uint8_t> out;
            void* slot = nullptr;
        };
        auto onDone = [](void* user, void* slot, const void* outPtr, size_t outBytes,
                         double /*gpuMs*/, bool ok) {
            AsyncCapture* cap = static_cast<AsyncCapture*>(user);
            std::lock_guard<std::mutex> lock(cap->m);
            cap->ok = ok;
            cap->slot = slot;
            cap->out.assign(static_cast<const uint8_t*>(outPtr),
                            static_cast<const uint8_t*>(outPtr) + outBytes);
            ++cap->fired;
            cap->cv.notify_all();
        };
        auto waitFired = [](AsyncCapture& cap, int n) {
            std::unique_lock<std::mutex> lock(cap.m);
            return cap.cv.wait_for(lock, std::chrono::seconds(5), [&] { return cap.fired >= n; });
        };

        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), frame.size() * sizeof(float));

        int outW = 0, outH = 0;
        ndi_stream::outputDims(srcW, srcH, 2, &outW, &outH);
        std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
        ndi_stream::downscaleRGBABox(frame.data(), srcW, srcH, srcW * 4, 2,
                                     small.data(), outW, outH);

        {
            std::vector<uint8_t> expected;
            referenceUYVY(small.data(), outW, outH, expected);
            AsyncCapture cap;
            bool submitted = metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                        srcW, srcH, srcW * 4, 2, outW, outH,
                                                        nullptr, onDone, &cap) == METAL_SUBMIT_OK;
            bool done = submitted && waitFired(cap, 1);
            check(done && cap.ok && cap.out.size() == expected.size() &&
                      compareBuffers(cap.out.data(), expected.data(), expected.size(), 0,
                                     "async UYVY"),
                  "async submit UYVY matches CPU reference (divisor 2)");
            if (cap.slot) metal_gpu_downscale_release(ctx, cap.slot);
        }

        {
            // Hold every slot: the 4 ring slots fill, the 5th submit refuses,
            // and releasing brings the ring back.
            AsyncCapture caps[5];
            int accepted = 0;
            for (int i = 0; i < 4; ++i) {
                if (metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                               srcW, srcH, srcW * 4, 2, outW, outH,
                                               nullptr, onDone, &caps[i]) == METAL_SUBMIT_OK) {
                    ++accepted;
                }
            }
            bool fifthRefused = metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                           srcW, srcH, srcW * 4, 2, outW, outH,
                                                           nullptr, onDone, &caps[4]) == METAL_SUBMIT_BUSY;
            bool allFired = true;
            for (int i = 0; i < 4; ++i) {
                allFired = allFired && waitFired(caps[i], 1);
                if (caps[i].slot) metal_gpu_downscale_release(ctx, caps[i].slot);
            }
            AsyncCapture after;
            bool afterOk = (metal_gpu_downscale_submit(ctx, nullptr, srcBuf,
                                                       srcW, srcH, srcW * 4, 2, outW, outH,
                                                       nullptr, onDone, &after) == METAL_SUBMIT_OK) &&
                           waitFired(after, 1);
            if (after.slot) metal_gpu_downscale_release(ctx, after.slot);
            check(accepted == 4 && fifthRefused && allFired && afterOk,
                  "slot ring: 4 in flight, 5th refused, released slots reusable");
        }

        metal_gpu_release_buffer(srcBuf);
    }

    // ------------------------------------------------------------------
    // STMap warp kernels (issue #7). The warp with an identity map must be
    // byte-identical to the plain downscale kernels (same taps, bilinear
    // weights degenerate to exact fetches at power-of-two dims), and an
    // arbitrary map must match the CPU reference warp within the same
    // rounding tolerance as the downscale kernels.
    // ------------------------------------------------------------------
    {
        // Identity map at source dims, row 0 = top, bottom-left v convention.
        auto identityMapUV = [](int w, int h) {
            std::vector<float> uv(static_cast<size_t>(w) * h * 2);
            for (int row = 0; row < h; ++row) {
                const float v = (h - 1 - row + 0.5f) / static_cast<float>(h);
                for (int x = 0; x < w; ++x) {
                    uv[(static_cast<size_t>(row) * w + x) * 2 + 0] = (x + 0.5f) / static_cast<float>(w);
                    uv[(static_cast<size_t>(row) * w + x) * 2 + 1] = v;
                }
            }
            return uv;
        };

        std::vector<float> frame;
        fillFrame(frame, srcW, srcH, srcW * 4);
        void* srcBuf = metal_gpu_create_shared_buffer(ctx, frame.data(), frame.size() * sizeof(float));

        std::vector<float> ident = identityMapUV(srcW, srcH);
        void* identBuf = metal_gpu_create_shared_buffer(ctx, ident.data(), ident.size() * sizeof(float));

        for (int divisor : {1, 2}) {
            int outW = 0, outH = 0;
            ndi_stream::outputDims(srcW, srcH, divisor, &outW, &outH);
            {
                std::vector<uint8_t> plain(static_cast<size_t>(outW) * outH * 2, 0);
                std::vector<uint8_t> warped(plain.size(), 1);
                bool ranPlain = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, srcBuf,
                                                                   srcW, srcH, srcW * 4, divisor,
                                                                   outW, outH, nullptr, plain.data());
                bool ranWarp = metal_gpu_buffer_warp_to_uyvy(ctx, nullptr, srcBuf,
                                                             srcW, srcH, srcW * 4,
                                                             identBuf, srcW, srcH, divisor,
                                                             outW, outH, nullptr, warped.data());
                char name[128];
                std::snprintf(name, sizeof(name),
                              "identity-map warp UYVY == downscale kernel (divisor %d)", divisor);
                check(ranPlain && ranWarp &&
                          std::memcmp(plain.data(), warped.data(), plain.size()) == 0, name);
            }
        }

        // Arbitrary smooth map at non-source dims, including an out-of-range
        // corner region (outside the lens circle -> black). No NaNs here:
        // GPU fast-math makes NaN comparisons formally undefined, and the
        // CPU seam test already pins NaN behavior.
        const int mapW = 48, mapH = 24;
        std::vector<float> mapUV(static_cast<size_t>(mapW) * mapH * 2);
        for (int row = 0; row < mapH; ++row) {
            for (int x = 0; x < mapW; ++x) {
                const float xn = (x + 0.5f) / mapW;
                const float yn = (mapH - 1 - row + 0.5f) / mapH;
                float u = xn * xn * 0.9f + 0.05f;
                float v = 0.1f + 0.75f * yn + 0.1f * xn;
                if (x < 4 && row < 4) { u = 1.5f; v = -0.25f; } // outside the circle
                mapUV[(static_cast<size_t>(row) * mapW + x) * 2 + 0] = u;
                mapUV[(static_cast<size_t>(row) * mapW + x) * 2 + 1] = v;
            }
        }
        void* mapBuf = metal_gpu_create_shared_buffer(ctx, mapUV.data(), mapUV.size() * sizeof(float));

        struct WarpCase { int divisor; int rowFloats; const char* name; };
        const WarpCase warpCases[] = {
            {1, srcW * 4, "divisor 1, tight rows"},
            {2, srcW * 4, "divisor 2, tight rows"},
            {2, (srcW + 2) * 4, "divisor 2, padded rows"},
        };
        for (const WarpCase& c : warpCases) {
            std::vector<float> wframe;
            fillFrame(wframe, srcW, srcH, c.rowFloats);
            void* wsrcBuf = metal_gpu_create_shared_buffer(ctx, wframe.data(),
                                                           wframe.size() * sizeof(float));
            int outW = 0, outH = 0;
            ndi_stream::outputDims(mapW, mapH, c.divisor, &outW, &outH);
            std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
            ndi_stmap::warpRGBABox(wframe.data(), srcW, srcH, c.rowFloats,
                                   mapUV.data(), mapW, mapH, c.divisor,
                                   small.data(), outW, outH);
            {
                std::vector<uint8_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
                referenceUYVY(small.data(), outW, outH, expected);
                bool ran = metal_gpu_buffer_warp_to_uyvy(ctx, nullptr, wsrcBuf,
                                                         srcW, srcH, c.rowFloats,
                                                         mapBuf, mapW, mapH, c.divisor,
                                                         outW, outH, nullptr, actual.data());
                char name[128];
                std::snprintf(name, sizeof(name), "warp UYVY matches CPU reference (%s)", c.name);
                check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 0, name),
                      name);
            }
            metal_gpu_release_buffer(wsrcBuf);
        }

        // Async warp submit: same slot ring, output bit-equal to the blocking
        // warp path.
        {
            struct AsyncCapture {
                std::mutex m;
                std::condition_variable cv;
                int fired = 0;
                bool ok = false;
                std::vector<uint8_t> out;
                void* slot = nullptr;
            };
            auto onDone = [](void* user, void* slot, const void* outPtr, size_t outBytes,
                             double /*gpuMs*/, bool ok) {
                AsyncCapture* cap = static_cast<AsyncCapture*>(user);
                std::lock_guard<std::mutex> lock(cap->m);
                cap->ok = ok;
                cap->slot = slot;
                cap->out.assign(static_cast<const uint8_t*>(outPtr),
                                static_cast<const uint8_t*>(outPtr) + outBytes);
                ++cap->fired;
                cap->cv.notify_all();
            };

            int outW = 0, outH = 0;
            ndi_stream::outputDims(mapW, mapH, 2, &outW, &outH);
            std::vector<uint8_t> blocking(static_cast<size_t>(outW) * outH * 2, 0);
            bool ranBlocking = metal_gpu_buffer_warp_to_uyvy(ctx, nullptr, srcBuf,
                                                             srcW, srcH, srcW * 4,
                                                             mapBuf, mapW, mapH, 2,
                                                             outW, outH, nullptr, blocking.data());
            AsyncCapture cap;
            bool submitted = metal_gpu_warp_submit(ctx, nullptr, srcBuf,
                                                   srcW, srcH, srcW * 4,
                                                   mapBuf, mapW, mapH, 2, outW, outH,
                                                   nullptr, onDone, &cap) == METAL_SUBMIT_OK;
            bool done = false;
            if (submitted) {
                std::unique_lock<std::mutex> lock(cap.m);
                done = cap.cv.wait_for(lock, std::chrono::seconds(5), [&] { return cap.fired >= 1; });
            }
            check(ranBlocking && done && cap.ok && cap.out.size() == blocking.size() &&
                      std::memcmp(cap.out.data(), blocking.data(), blocking.size()) == 0,
                  "async warp submit matches blocking warp output");
            if (cap.slot) metal_gpu_downscale_release(ctx, cap.slot);
        }

        // A missing/undersized map buffer must refuse as INVALID (the caller
        // falls back to the CPU warp), never crash.
        {
            int outW = 0, outH = 0;
            ndi_stream::outputDims(mapW, mapH, 1, &outW, &outH);
            std::vector<uint8_t> outBytes(static_cast<size_t>(outW) * outH * 2, 0);
            bool nullRefused = !metal_gpu_buffer_warp_to_uyvy(ctx, nullptr, srcBuf,
                                                              srcW, srcH, srcW * 4,
                                                              nullptr, mapW, mapH, 1,
                                                              outW, outH, nullptr, outBytes.data());
            void* tiny = metal_gpu_create_shared_buffer(ctx, nullptr, 16);
            auto noopDone = [](void*, void*, const void*, size_t, double, bool) {};
            bool tinyRefused = metal_gpu_warp_submit(ctx, nullptr, srcBuf,
                                                     srcW, srcH, srcW * 4,
                                                     tiny, mapW, mapH, 1, outW, outH,
                                                     nullptr, noopDone, nullptr) == METAL_SUBMIT_INVALID;
            metal_gpu_release_buffer(tiny);
            check(nullRefused && tinyRefused, "warp refuses null/undersized map buffers as invalid");
        }

        metal_gpu_release_buffer(mapBuf);
        metal_gpu_release_buffer(identBuf);
        metal_gpu_release_buffer(srcBuf);
    }

    // ------------------------------------------------------------------
    // Output Color stage (issue #57 Part 2): every preset pair and 3D LUTs,
    // through the downscale and warp kernels, blocking and async, must be
    // byte-identical to the CPU converter running ndi_color::applyTransform.
    // ------------------------------------------------------------------
    {
        // A frame carrying the full 33^3 sweep, then a fine grey ramp.
        const int sw = 198, sh = 182;
        std::vector<float> sweep(static_cast<size_t>(sw) * sh * 4);
        for (int k = 0; k < sw * sh; ++k) {
            float* px = sweep.data() + static_cast<size_t>(k) * 4;
            if (k < 33 * 33 * 33) {
                px[0] = (k % 33) / 32.0f;
                px[1] = (k / 33 % 33) / 32.0f;
                px[2] = (k / 1089) / 32.0f;
            } else {
                px[0] = px[1] = px[2] = static_cast<float>(k - 33 * 33 * 33) / (sw * sh - 33 * 33 * 33 - 1);
            }
            px[3] = 1.0f;
        }
        void* sweepBuf = metal_gpu_create_shared_buffer(ctx, sweep.data(), sweep.size() * sizeof(float));

        auto cpuRef = [](const float* src, int w, int h, const ndi_color::Transform& t,
                         std::vector<uint8_t>& out) {
            out.assign(static_cast<size_t>(w) * h * 2, 0);
            ndi_uyvy::convertRGBAToUYVYFlipped(src, w, h, out.data(),
                                               [&](float* rgb) { ndi_color::applyTransform(t, rgb); });
        };
        struct Uploaded {
            void* buffer = nullptr;
            metal_color_desc desc = {nullptr, 0, 0};
        };
        auto upload = [&](const ndi_color::Transform& t) {
            Uploaded u;
            u.buffer = metal_gpu_create_shared_buffer(ctx, t.data.data(), t.data.size() * sizeof(float));
            u.desc.buffer = u.buffer;
            u.desc.mode = t.mode;
            u.desc.lutSize = t.lutSize;
            return u;
        };

        // Every preset pair over the sweep frame, divisor 1.
        int pairs = 0, exact = 0;
        for (int a = 0; a < ndi_color::kDisplaySpaceCount; ++a) {
            for (int b = 0; b < ndi_color::kDisplaySpaceCount; ++b) {
                if (a == b) continue;
                ndi_color::Transform t;
                ndi_color::buildPreset(a, b, &t);
                Uploaded u = upload(t);
                std::vector<uint8_t> expected, actual(static_cast<size_t>(sw) * sh * 2, 0);
                cpuRef(sweep.data(), sw, sh, t, expected);
                const bool ran = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1,
                                                                    sw, sh, &u.desc, actual.data());
                ++pairs;
                if (ran && compareBuffers(actual.data(), expected.data(), expected.size(), 0,
                                          ndi_color::displaySpaceName(b))) {
                    ++exact;
                }
                metal_gpu_release_buffer(u.buffer);
            }
        }
        {
            char name[128];
            std::snprintf(name, sizeof(name), "all %d preset pairs byte-identical to the CPU over a 33^3 sweep",
                          pairs);
            check(pairs == 42 && exact == pairs, name);
        }

        // Box average THEN transform: divisor 2, padded rows, one preset.
        {
            const int rowFloats = (srcW + 2) * 4;
            std::vector<float> frame;
            fillFrame(frame, srcW, srcH, rowFloats);
            void* buf = metal_gpu_create_shared_buffer(ctx, frame.data(), frame.size() * sizeof(float));
            int outW = 0, outH = 0;
            ndi_stream::outputDims(srcW, srcH, 2, &outW, &outH);
            std::vector<float> small(static_cast<size_t>(outW) * outH * 4);
            ndi_stream::downscaleRGBABox(frame.data(), srcW, srcH, rowFloats, 2, small.data(), outW, outH);
            ndi_color::Transform t;
            ndi_color::buildPreset(ndi_color::kRec709Gamma24, ndi_color::kP3D65Gamma22, &t);
            Uploaded u = upload(t);
            std::vector<uint8_t> expected, actual(static_cast<size_t>(outW) * outH * 2, 0);
            cpuRef(small.data(), outW, outH, t, expected);
            const bool ran = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, buf, srcW, srcH, rowFloats, 2,
                                                                outW, outH, &u.desc, actual.data());
            check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 0, "preset divisor 2"),
                  "preset after a divisor-2 box on padded rows matches the CPU");
            metal_gpu_release_buffer(u.buffer);
            metal_gpu_release_buffer(buf);
        }

        // LUTs: an identity cube is the passthrough bytes; a curved cube with a
        // wide domain, and a 65-point one, match the CPU exactly.
        auto lattice = [](int n, float dmin, float dmax, bool curved) {
            ndi_cube::CubeLUT lut;
            lut.size = n;
            for (int c = 0; c < 3; ++c) {
                lut.domainMin[c] = dmin;
                lut.domainMax[c] = dmax;
            }
            const double step = (static_cast<double>(dmax) - dmin) / (n - 1);
            for (int b = 0; b < n; ++b)
                for (int g = 0; g < n; ++g)
                    for (int r = 0; r < n; ++r) {
                        const double R = dmin + r * step, G = dmin + g * step, B = dmin + b * step;
                        if (curved) {
                            lut.rgb.push_back(static_cast<float>(0.9 * std::pow(std::fabs(R), 1.7) + 0.1 * G));
                            lut.rgb.push_back(static_cast<float>(std::sqrt(std::fabs(G)) * 0.8 + 0.2 * B));
                            lut.rgb.push_back(static_cast<float>(1.05 * B - 0.03 * R));
                        } else {
                            lut.rgb.push_back(static_cast<float>(R));
                            lut.rgb.push_back(static_cast<float>(G));
                            lut.rgb.push_back(static_cast<float>(B));
                        }
                    }
            return lut;
        };
        {
            ndi_color::Transform ident;
            ndi_color::buildLut(lattice(33, 0.0f, 1.0f, false), &ident);
            Uploaded u = upload(ident);
            std::vector<uint8_t> plain(static_cast<size_t>(sw) * sh * 2, 0), viaLut(plain.size(), 1);
            const bool ranPlain = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1,
                                                                     sw, sh, nullptr, plain.data());
            const bool ranLut = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1,
                                                                   sw, sh, &u.desc, viaLut.data());
            check(ranPlain && ranLut && std::memcmp(plain.data(), viaLut.data(), plain.size()) == 0,
                  "identity 33-point LUT streams the passthrough bytes");
            metal_gpu_release_buffer(u.buffer);
        }
        for (int n : {17, 65}) {
            ndi_color::Transform t;
            ndi_color::buildLut(lattice(n, -0.1f, 1.2f, true), &t);
            Uploaded u = upload(t);
            std::vector<uint8_t> expected, actual(static_cast<size_t>(sw) * sh * 2, 0);
            cpuRef(sweep.data(), sw, sh, t, expected);
            const bool ran = metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1,
                                                                sw, sh, &u.desc, actual.data());
            char name[128];
            std::snprintf(name, sizeof(name), "curved %d-point LUT, wide domain, matches the CPU byte-for-byte", n);
            check(ran && compareBuffers(actual.data(), expected.data(), expected.size(), 0, name), name);
            metal_gpu_release_buffer(u.buffer);
        }

        // Warp + colour, and the async path: a smooth map warp with a preset
        // matches the CPU warp + transform; async equals blocking.
        {
            const int mw = 120, mh = 96;
            std::vector<float> mapUV(static_cast<size_t>(mw) * mh * 2);
            for (int row = 0; row < mh; ++row)
                for (int x = 0; x < mw; ++x) {
                    const float xn = (x + 0.5f) / mw, yn = (mh - 1 - row + 0.5f) / mh;
                    mapUV[(static_cast<size_t>(row) * mw + x) * 2 + 0] = 0.05f + 0.9f * xn * xn;
                    mapUV[(static_cast<size_t>(row) * mw + x) * 2 + 1] = 0.1f + 0.75f * yn + 0.1f * xn;
                }
            void* mapBuf = metal_gpu_create_shared_buffer(ctx, mapUV.data(), mapUV.size() * sizeof(float));
            ndi_color::Transform t;
            ndi_color::buildPreset(ndi_color::kRec2020Gamma24, ndi_color::kSRGB, &t);
            Uploaded u = upload(t);
            int ow = 0, oh = 0;
            ndi_stream::outputDims(mw, mh, 2, &ow, &oh);
            std::vector<float> small(static_cast<size_t>(ow) * oh * 4);
            ndi_stmap::warpRGBABox(sweep.data(), sw, sh, sw * 4, mapUV.data(), mw, mh, 2, small.data(), ow, oh);
            std::vector<uint8_t> expected, warped(static_cast<size_t>(ow) * oh * 2, 1);
            cpuRef(small.data(), ow, oh, t, expected);
            const bool ranWarp = metal_gpu_buffer_warp_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, mapBuf,
                                                               mw, mh, 2, ow, oh, &u.desc, warped.data());
            check(ranWarp && compareBuffers(warped.data(), expected.data(), expected.size(), 0, "warp + preset"),
                  "warp (divisor 2) + preset matches the CPU warp + transform");

            struct Cap {
                std::mutex m;
                std::condition_variable cv;
                bool fired = false, ok = false;
                std::vector<uint8_t> out;
                void* slot = nullptr;
            } cap;
            auto onDone = [](void* user, void* slot, const void* outPtr, size_t outBytes, double, bool ok) {
                Cap* c = static_cast<Cap*>(user);
                std::lock_guard<std::mutex> lock(c->m);
                c->ok = ok;
                c->slot = slot;
                c->out.assign(static_cast<const uint8_t*>(outPtr), static_cast<const uint8_t*>(outPtr) + outBytes);
                c->fired = true;
                c->cv.notify_all();
            };
            bool done = false;
            if (metal_gpu_warp_submit(ctx, nullptr, sweepBuf, sw, sh, sw * 4, mapBuf, mw, mh, 2, ow, oh, &u.desc,
                                      onDone, &cap) == METAL_SUBMIT_OK) {
                std::unique_lock<std::mutex> lock(cap.m);
                done = cap.cv.wait_for(lock, std::chrono::seconds(5), [&] { return cap.fired; });
            }
            check(done && cap.ok && cap.out == warped, "async warp + preset matches the blocking path");
            if (cap.slot) metal_gpu_downscale_release(ctx, cap.slot);

            // CPU-buffer entry point carries the transform too.
            std::vector<uint8_t> full, cpuBuf(static_cast<size_t>(sw) * sh * 2, 2);
            cpuRef(sweep.data(), sw, sh, t, full);
            const bool ranCpuBuf = metal_gpu_convert_rgba_to_uyvy(ctx, sweep.data(), cpuBuf.data(), sw, sh, &u.desc);
            check(ranCpuBuf && cpuBuf == full, "CPU-buffer convert + preset matches the CPU");

            metal_gpu_release_buffer(u.buffer);
            metal_gpu_release_buffer(mapBuf);
        }

        // A descriptor whose buffer can't hold its transform is refused.
        {
            void* tiny = metal_gpu_create_shared_buffer(ctx, nullptr, 64);
            metal_color_desc lutDesc = {tiny, ndi_color::kModeLut3D, 33};
            metal_color_desc matDesc = {tiny, ndi_color::kModeMatrix, 0};
            metal_color_desc noBuf = {nullptr, ndi_color::kModeMatrix, 0};
            metal_color_desc badMode = {tiny, 7, 0};
            std::vector<uint8_t> out(static_cast<size_t>(sw) * sh * 2, 0);
            auto noopDone = [](void*, void*, const void*, size_t, double, bool) {};
            const bool refused =
                !metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1, sw, sh, &lutDesc,
                                                    out.data()) &&
                !metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1, sw, sh, &matDesc,
                                                    out.data()) &&
                !metal_gpu_buffer_downscale_to_uyvy(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1, sw, sh, &badMode,
                                                    out.data()) &&
                metal_gpu_downscale_submit(ctx, nullptr, sweepBuf, sw, sh, sw * 4, 1, sw, sh, &noBuf, noopDone,
                                           nullptr) == METAL_SUBMIT_INVALID;
            check(refused, "undersized, missing or unknown colour descriptors are refused");
            metal_gpu_release_buffer(tiny);
        }

        metal_gpu_release_buffer(sweepBuf);
    }

    metal_gpu_shutdown(ctx);

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All Metal fast-path tests passed\n");
    return 0;
}
