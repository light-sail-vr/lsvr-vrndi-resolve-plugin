// Tests for the shared UYVY encoder seam (src/UYVYEncode.h): BT.709
// limited-range quantization (issue #57 Part 1) and the flipping full-frame
// converter that the CPU path runs and the Metal/CUDA kernel tests use as
// their oracle. Expected codes are the published BT.709 8-bit values
// (16-235 luma, 16-240 chroma, 100% color bars), not recomputed here.
// Build & run: make test

#include "UYVYEncode.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;

static void expectInt(long long actual, long long expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: %lld\n  actual:   %lld\n", name, expected, actual);
    } else {
        std::printf("ok   %s\n", name);
    }
}

// Encode one flat colour as a macropixel; returns U, Y1, V, Y2.
static void encodeFlat(float r, float g, float b, uint8_t out[4])
{
    ndi_uyvy::encodeMacropixel(r, g, b, r, g, b, out);
}

int main()
{
    uint8_t m[4];

    // --- Levels: the acceptance values of issue #57 Part 1 ---
    encodeFlat(0.0f, 0.0f, 0.0f, m);
    expectInt(m[1], 16, "black -> Y=16");
    expectInt(m[3], 16, "black -> Y2=16");
    expectInt(m[0], 128, "black -> Cb=128");
    expectInt(m[2], 128, "black -> Cr=128");

    encodeFlat(1.0f, 1.0f, 1.0f, m);
    expectInt(m[1], 235, "white -> Y=235");
    expectInt(m[0], 128, "white -> Cb=128");
    expectInt(m[2], 128, "white -> Cr=128");

    encodeFlat(0.5f, 0.5f, 0.5f, m);
    expectInt(m[1], 126, "50% grey -> Y=126 (125.5 rounds half up)");
    expectInt(m[0], 128, "50% grey -> neutral Cb=128");
    expectInt(m[2], 128, "50% grey -> neutral Cr=128");

    // The patches the old full-range encoder crushed/clipped: 16/255 and
    // 235/255 must land inside the legal range so NDI's decoder returns them.
    encodeFlat(16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f, m);
    expectInt(m[1], 30, "16/255 grey -> Y=30 (decodes back to 16)");
    encodeFlat(235.0f / 255.0f, 235.0f / 255.0f, 235.0f / 255.0f, m);
    expectInt(m[1], 218, "235/255 grey -> Y=218 (decodes back to 235)");

    // Neutral chroma must sit on 128 across the whole grey ramp (the old
    // truncating encoder landed on 127 — the green cast).
    {
        int off = 0;
        for (int i = 0; i <= 255; ++i) {
            const float v = static_cast<float>(i) / 255.0f;
            encodeFlat(v, v, v, m);
            if (m[0] != 128 || m[2] != 128) ++off;
        }
        expectInt(off, 0, "every grey 0..255 has Cb=Cr=128");
    }

    // --- 100% primaries: BT.709 8-bit colour-bar codes (Y, Cb, Cr) ---
    encodeFlat(1.0f, 0.0f, 0.0f, m);
    expectInt(m[1], 63, "red -> Y=63");
    expectInt(m[0], 102, "red -> Cb=102");
    expectInt(m[2], 240, "red -> Cr=240");
    encodeFlat(0.0f, 1.0f, 0.0f, m);
    expectInt(m[1], 173, "green -> Y=173");
    expectInt(m[0], 42, "green -> Cb=42");
    expectInt(m[2], 26, "green -> Cr=26");
    encodeFlat(0.0f, 0.0f, 1.0f, m);
    expectInt(m[1], 32, "blue -> Y=32");
    expectInt(m[0], 240, "blue -> Cb=240");
    expectInt(m[2], 118, "blue -> Cr=118");

    // --- Full-frame converter: clamp, vertical flip, 4:2:2 chroma pairing ---
    {
        // 2x2 OFX frame, bottom-up rows: row 0 (bottom) = black | white,
        // row 1 (top) = red | blue, plus out-of-range values that must clamp.
        const float src[2 * 2 * 4] = {
            -0.2f, -0.2f, -0.2f, 1.0f,   1.5f, 1.5f, 1.5f, 1.0f,   // bottom row
             1.0f,  0.0f,  0.0f, 1.0f,   0.0f, 0.0f, 1.0f, 1.0f,   // top row
        };
        std::vector<uint8_t> out(2 * 2 * 2, 0);
        ndi_uyvy::convertRGBAToUYVYFlipped(src, 2, 2, out.data());
        // Output row 0 is the TOP source row (red | blue).
        expectInt(out[1], 63, "flip: top-left output luma is red");
        expectInt(out[3], 32, "flip: top-right output luma is blue");
        // Chroma of the red+blue pair: average (0.5, 0, 0.5) -> magenta-ish
        // Cb = 128 + 224*0.192714 = 171.2, Cr = 128 + 224*0.227076 = 178.9.
        expectInt(out[0], 171, "pair chroma Cb from averaged red+blue");
        expectInt(out[2], 179, "pair chroma Cr from averaged red+blue");
        // Output row 1 is the bottom source row, clamped to black | white.
        expectInt(out[5], 16, "clamp: -0.2 encodes as black");
        expectInt(out[7], 235, "clamp: 1.5 encodes as white");
        expectInt(out[4], 128, "black+white pair is neutral (Cb)");
        expectInt(out[6], 128, "black+white pair is neutral (Cr)");
    }

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All UYVY encoder tests passed\n");
    return 0;
}
