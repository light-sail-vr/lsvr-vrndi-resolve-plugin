#ifndef _UYVYEncode_h_
#define _UYVYEncode_h_

/*
  Shared UYVY encoder (issue #57 Part 1): BT.709 Y'CbCr at LIMITED range —
  Y = 16 + 219·Y', Cb/Cr = 128 + 224·C', clamped, rounded to nearest.

  NDI is a limited-range format: its own RGBA→UYVY encode puts black at Y=16
  and white at Y=235, and its UYVY→RGB decode clips Y≤16 to 0 and Y≥235 to
  255 (measured with SDK 6.0.1; "full-range signals are not supported by NDI
  Tools", SDK docs p. 40). Every encoder before this header wrote full range
  (Y = 255·Y', truncated), so receivers crushed everything below ~6%,
  clipped everything above ~92% and tinted neutrals green (chroma 127).

  Kept free of OFX, NDI and GPU dependencies so it is unit-testable without
  a host (tests/test_uyvy_encode.cpp). This is the ONE definition of the
  arithmetic: the CPU path calls it, the Metal (emit_uyvy) and CUDA
  (cudaEmitUYVY) kernels spell the same expression tree, and the GPU kernel
  tests use convertRGBAToUYVYFlipped as their byte-exact oracle.

  Byte-identity contract: every multiply that feeds an add is an explicit
  fma. The Metal compiler contracts a*b+c into an fma on its own (even
  across statements under its default fast math), MSVC never does, and
  clang does within one expression on arm64 — spelling the fma out is the
  only way all three backends round identically. Keep it that way when
  editing: no bare `a * b + c` anywhere in this file or its kernel mirrors.
*/

#include <cmath>
#include <cstdint>

namespace ndi_uyvy {

// BT.709 (Kr = 0.2126, Kb = 0.0722). Chroma rows are (B - Y')/1.8556 and
// (R - Y')/1.5748 expanded, so each row sums to zero and neutrals have
// zero chroma.
constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;
constexpr float kCbR = -0.11457211f;
constexpr float kCbG = -0.38542789f;
constexpr float kCbB = 0.5f;
constexpr float kCrR = 0.5f;
constexpr float kCrG = -0.45415291f;
constexpr float kCrB = -0.04584709f;

// Limited-range quantization: scale·v + offset, clamped to [lo, hi], then
// rounded half-up (+0.5 and truncate — the value is never negative here).
inline uint8_t quantize(float v, float scale, float offset, float lo, float hi)
{
    const float q = std::fmin(std::fmax(std::fma(scale, v, offset), lo), hi);
    return static_cast<uint8_t>(q + 0.5f);
}

inline uint8_t lumaCode(float y) { return quantize(y, 219.0f, 16.0f, 16.0f, 235.0f); }
inline uint8_t chromaCode(float c) { return quantize(c, 224.0f, 128.0f, 16.0f, 240.0f); }

// Two horizontally adjacent pixels (R'G'B' already clamped to [0,1]) → one
// UYVY macropixel: out = {Cb, Y1, Cr, Y2}. Chroma comes from the pair's
// average (4:2:2).
inline void encodeMacropixel(float r1, float g1, float b1,
                             float r2, float g2, float b2, uint8_t out[4])
{
    const float y1 = std::fma(kLumaR, r1, std::fma(kLumaG, g1, kLumaB * b1));
    const float y2 = std::fma(kLumaR, r2, std::fma(kLumaG, g2, kLumaB * b2));
    const float ar = (r1 + r2) * 0.5f;
    const float ag = (g1 + g2) * 0.5f;
    const float ab = (b1 + b2) * 0.5f;
    const float cb = std::fma(kCbR, ar, std::fma(kCbG, ag, kCbB * ab));
    const float cr = std::fma(kCrR, ar, std::fma(kCrG, ag, kCrB * ab));
    out[0] = chromaCode(cb);
    out[1] = lumaCode(y1);
    out[2] = chromaCode(cr);
    out[3] = lumaCode(y2);
}

inline float clamp01(float v) { return std::fmax(0.0f, std::fmin(1.0f, v)); }

// Full-frame converter: tightly packed float RGBA, bottom-up rows (OFX) →
// top-down UYVY (NDI), width*height*2 bytes. Each pixel is clamped to [0,1],
// handed to `transform(float rgb[3])` (the Output Color stage, issue #57
// Part 2 — it must keep values in [0,1]), then encoded. An odd trailing
// pixel pairs with itself and writes only its Cb/Y half, so an odd width can
// never write past its row (the pre-#57 converter overran by two bytes).
struct NoTransform {
    void operator()(float*) const {}
};

template <typename PixelTransform>
inline void convertRGBAToUYVYFlipped(const float* src, int width, int height, uint8_t* dst,
                                     const PixelTransform& transform)
{
    for (int y = 0; y < height; ++y) {
        const int srcRow = height - 1 - y; // OFX is bottom-up, NDI top-down
        for (int x = 0; x < width; x += 2) {
            const float* p1 = src + (static_cast<long long>(srcRow) * width + x) * 4;
            const bool hasSecond = (x + 1 < width);
            const float* p2 = hasSecond ? p1 + 4 : p1;
            float a[3] = {clamp01(p1[0]), clamp01(p1[1]), clamp01(p1[2])};
            float b[3] = {clamp01(p2[0]), clamp01(p2[1]), clamp01(p2[2])};
            transform(a);
            transform(b);
            uint8_t m[4];
            encodeMacropixel(a[0], a[1], a[2], b[0], b[1], b[2], m);
            uint8_t* out = dst + (static_cast<long long>(y) * width + x) * 2;
            out[0] = m[0];
            out[1] = m[1];
            if (hasSecond) {
                out[2] = m[2];
                out[3] = m[3];
            }
        }
    }
}

inline void convertRGBAToUYVYFlipped(const float* src, int width, int height, uint8_t* dst)
{
    convertRGBAToUYVYFlipped(src, width, height, dst, NoTransform());
}

} // namespace ndi_uyvy

#endif
