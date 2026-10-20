#ifndef _OutputColor_h_
#define _OutputColor_h_

/*
  Output-colour transforms (issue #57 Part 2): what the Output Color group
  applies to every streamed pixel after the box/warp sample and before the
  UYVY encode (src/UYVYEncode.h). The node's own output back to Resolve is
  never touched.

  Two kinds, flattened into one float array the GPU kernels read verbatim
  (Transform::data, uploaded through the same per-device cache as STMaps):

  - Preset (kModeMatrix): display-referred → display-referred between the
    seven display spaces below. Decode the input transfer, 3x3 primaries
    matrix (all D65, no chromatic adaptation), clip to [0,1], encode the
    output transfer. Gamma 2.2/2.4/2.6 are pure power functions (as in
    Resolve); sRGB and Display P3 use the piecewise sRGB curve. Out-of-gamut
    values clip — there is no gamut mapping. Layout:
      [decode table (kTableSize)] [encode table (kTableSize)] [3x3 row-major]
  - LUT (kModeLut3D): a parsed .cube (src/CubeLUT.h), trilinear. Layout:
      [scale r,g,b] [bias r,g,b] [N^3 RGB triples, red fastest]
    where lattice coordinate = fma(x, scale, bias) maps DOMAIN_MIN..MAX onto
    0..N-1.

  Transfer curves are TABLES, not pow(): the Metal kernels compile under fast
  math, where pow() is approximate, and CPU/Metal/CUDA must agree to the
  byte. The tables are indexed by the float's own bit pattern — exponent plus
  the top kTableMantBits of the mantissa — so each octave from 2^-24 to 1 gets
  2^kTableMantBits linear segments: uniform RELATIVE precision, which the
  infinite slope of x^(1/2.4) near black needs (a uniform table is several
  codes off there). Below 2^-24 one linear segment runs from f(0).

  Byte-identity contract (same as UYVYEncode.h): every multiply that feeds an
  add is an explicit fma, every other product is exact or unfusable, and the
  bit-pattern index is integer arithmetic. The Metal (apply_color) and CUDA
  (cudaApplyColor) kernels spell exactly these steps; make test-metal and
  test_cuda_downscale compare them byte-for-byte.

  Kept free of OFX/NDI/GPU dependencies (tests/test_output_color.cpp).
*/

#include "CubeLUT.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ndi_color {

// Display spaces, in UI choice order. Projects save choice INDICES, so this
// list is append-only.
enum DisplaySpace {
    kRec709Gamma24 = 0,
    kRec709Gamma22,
    kSRGB,
    kP3D65Gamma22,
    kP3D65Gamma26,
    kDisplayP3,
    kRec2020Gamma24,
    kDisplaySpaceCount
};

inline const char* displaySpaceName(int space)
{
    static const char* const kNames[kDisplaySpaceCount] = {
        "Rec.709 Gamma 2.4", "Rec.709 Gamma 2.2", "sRGB", "P3-D65 Gamma 2.2",
        "P3-D65 Gamma 2.6", "Display P3", "Rec.2020 Gamma 2.4"};
    return (space >= 0 && space < kDisplaySpaceCount) ? kNames[space] : "unknown";
}

// Compact form for Stream Status ("color: Rec.709 G2.4 -> P3-D65 G2.2").
inline const char* displaySpaceShortName(int space)
{
    static const char* const kNames[kDisplaySpaceCount] = {
        "Rec.709 G2.4", "Rec.709 G2.2", "sRGB", "P3-D65 G2.2", "P3-D65 G2.6", "Display P3", "Rec.2020 G2.4"};
    return (space >= 0 && space < kDisplaySpaceCount) ? kNames[space] : "unknown";
}

enum Primaries { kPrimBT709 = 0, kPrimP3D65, kPrimBT2020 };
enum Curve { kCurveGamma22 = 0, kCurveGamma24, kCurveGamma26, kCurveSRGB };

inline Primaries primariesOf(int space)
{
    switch (space) {
        case kP3D65Gamma22:
        case kP3D65Gamma26:
        case kDisplayP3: return kPrimP3D65;
        case kRec2020Gamma24: return kPrimBT2020;
        default: return kPrimBT709;
    }
}

inline Curve curveOf(int space)
{
    switch (space) {
        case kRec709Gamma22:
        case kP3D65Gamma22: return kCurveGamma22;
        case kP3D65Gamma26: return kCurveGamma26;
        case kSRGB:
        case kDisplayP3: return kCurveSRGB;
        default: return kCurveGamma24;
    }
}

enum Mode { kModeNone = 0, kModeMatrix = 1, kModeLut3D = 2 };

// --- transfer tables ------------------------------------------------------------
constexpr int kTableMantBits = 6;                       // 64 segments per octave
constexpr int kTableOctaves = 24;                       // 2^-24 .. 1
constexpr int kTableShift = 23 - kTableMantBits;        // float bits >> shift = segment number
constexpr uint32_t kTableBase = (127u - kTableOctaves) << kTableMantBits;  // segment of 2^-24
constexpr uint32_t kTableMantMask = (1u << kTableShift) - 1u;
constexpr float kTableMantScale = 1.0f / static_cast<float>(1u << kTableShift);  // exact 2^-17
constexpr float kTableFloor = 1.0f / 16777216.0f;      // 2^-24
constexpr float kTableFloorInv = 16777216.0f;          // 2^24
// Entry 0 = f(0); entry k >= 1 = f(start of segment k-1); one spare entry
// past 1.0 so the interpolation at exactly 1.0 (weight 0) stays in bounds.
constexpr int kTableSize = (kTableOctaves << kTableMantBits) + 3;
constexpr uint32_t kTableLastIndex = static_cast<uint32_t>(kTableSize) - 2u;

// --- flattened layouts -------------------------------------------------------------
constexpr int kDecodeOffset = 0;
constexpr int kEncodeOffset = kTableSize;
constexpr int kMatrixOffset = 2 * kTableSize;
constexpr int kMatrixDataSize = 2 * kTableSize + 9;
constexpr int kLutScaleOffset = 0;
constexpr int kLutBiasOffset = 3;
constexpr int kLutDataOffset = 6;

struct Transform {
    int mode = kModeNone;
    int lutSize = 0;          // kModeLut3D only
    std::vector<float> data;  // see the layouts above
};

// Same operand order as ndi_uyvy::clamp01 and the kernels' clamps, so even
// a NaN clamps identically on every backend.
inline float clamp01(float v) { return std::fmax(0.0f, std::fmin(1.0f, v)); }

inline uint32_t floatBits(float x)
{
    uint32_t u;
    std::memcpy(&u, &x, sizeof(u));
    return u;
}

// x is expected in [0,1]; anything else (NaN included) still indexes inside
// the table, so hostile input can never read out of bounds.
inline float tableLookup(const float* table, float x)
{
    if (x < kTableFloor) {
        const float t = x * kTableFloorInv;  // exact: power-of-two scale
        return std::fma(table[1] - table[0], t, table[0]);
    }
    const uint32_t bits = floatBits(x);
    uint32_t i = (bits >> kTableShift) - kTableBase + 1u;
    if (i > kTableLastIndex) i = kTableLastIndex;
    const float f = static_cast<float>(bits & kTableMantMask) * kTableMantScale;  // exact
    return std::fma(table[i + 1] - table[i], f, table[i]);
}

namespace detail {

inline double decodeCurve(Curve c, double v)
{
    switch (c) {
        case kCurveGamma22: return std::pow(v, 2.2);
        case kCurveGamma26: return std::pow(v, 2.6);
        case kCurveSRGB: return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        default: return std::pow(v, 2.4);
    }
}

inline double encodeCurve(Curve c, double l)
{
    switch (c) {
        case kCurveGamma22: return std::pow(l, 1.0 / 2.2);
        case kCurveGamma26: return std::pow(l, 1.0 / 2.6);
        case kCurveSRGB: return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
        default: return std::pow(l, 1.0 / 2.4);
    }
}

inline void fillTable(float* table, Curve c, bool encode)
{
    for (int k = 0; k < kTableSize; ++k) {
        double x = 0.0;
        if (k > 0) {
            const int j = k - 1;
            const int exponent = -kTableOctaves + (j >> kTableMantBits);
            const double mant = 1.0 + static_cast<double>(j & ((1 << kTableMantBits) - 1)) /
                                          static_cast<double>(1 << kTableMantBits);
            x = std::ldexp(mant, exponent);
        }
        table[k] = static_cast<float>(encode ? encodeCurve(c, x) : decodeCurve(c, x));
    }
}

inline void invert3(const double m[9], double out[9])
{
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    out[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    out[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    out[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / det;
}

// Linear RGB → CIE XYZ for a primaries set with a D65 white point.
inline void rgbToXyz(Primaries p, double m[9])
{
    static const double kChroma[3][6] = {
        {0.640, 0.330, 0.300, 0.600, 0.150, 0.060},  // BT.709 / sRGB
        {0.680, 0.320, 0.265, 0.690, 0.150, 0.060},  // P3-D65 / Display P3
        {0.708, 0.292, 0.170, 0.797, 0.131, 0.046},  // BT.2020
    };
    const double* c = kChroma[p];
    const double wx = 0.3127, wy = 0.3290;
    double P[9];
    for (int i = 0; i < 3; ++i) {
        const double x = c[i * 2], y = c[i * 2 + 1];
        P[0 * 3 + i] = x / y;
        P[1 * 3 + i] = 1.0;
        P[2 * 3 + i] = (1.0 - x - y) / y;
    }
    double Pi[9];
    invert3(P, Pi);
    const double W[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
    for (int i = 0; i < 3; ++i) {
        const double s = Pi[i * 3] * W[0] + Pi[i * 3 + 1] * W[1] + Pi[i * 3 + 2] * W[2];
        for (int r = 0; r < 3; ++r) m[r * 3 + i] = P[r * 3 + i] * s;
    }
}

} // namespace detail

// Build the preset for inSpace → outSpace. Same space = kModeNone: the
// stream stays bit-identical to passthrough. False for out-of-range spaces.
inline bool buildPreset(int inSpace, int outSpace, Transform* out)
{
    *out = Transform();
    if (inSpace < 0 || inSpace >= kDisplaySpaceCount || outSpace < 0 || outSpace >= kDisplaySpaceCount) {
        return false;
    }
    if (inSpace == outSpace) {
        return true;
    }
    out->mode = kModeMatrix;
    out->data.assign(kMatrixDataSize, 0.0f);
    detail::fillTable(out->data.data() + kDecodeOffset, curveOf(inSpace), false);
    detail::fillTable(out->data.data() + kEncodeOffset, curveOf(outSpace), true);
    double toXyz[9], outToXyz[9], fromXyz[9];
    detail::rgbToXyz(primariesOf(inSpace), toXyz);
    detail::rgbToXyz(primariesOf(outSpace), outToXyz);
    detail::invert3(outToXyz, fromXyz);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const double v = fromXyz[r * 3] * toXyz[c] + fromXyz[r * 3 + 1] * toXyz[3 + c] +
                             fromXyz[r * 3 + 2] * toXyz[6 + c];
            out->data[kMatrixOffset + r * 3 + c] = static_cast<float>(v);
        }
    }
    return true;
}

// Flatten a parsed .cube. False for an empty/invalid LUT.
inline bool buildLut(const ndi_cube::CubeLUT& lut, Transform* out)
{
    *out = Transform();
    const size_t n = static_cast<size_t>(lut.size);
    if (lut.size < ndi_cube::kMinSize || lut.size > ndi_cube::kMaxSize || lut.rgb.size() != n * n * n * 3) {
        return false;
    }
    out->mode = kModeLut3D;
    out->lutSize = lut.size;
    out->data.resize(kLutDataOffset + lut.rgb.size());
    for (int c = 0; c < 3; ++c) {
        const double scale = (lut.size - 1) / (static_cast<double>(lut.domainMax[c]) - lut.domainMin[c]);
        out->data[kLutScaleOffset + c] = static_cast<float>(scale);
        out->data[kLutBiasOffset + c] = static_cast<float>(-lut.domainMin[c] * scale);
    }
    std::memcpy(out->data.data() + kLutDataOffset, lut.rgb.data(), lut.rgb.size() * sizeof(float));
    return true;
}

// Per-pixel apply on raw flattened data (what the GPU kernels mirror). rgb
// is R'G'B' clamped to [0,1] on entry and stays in [0,1].
inline void applyTransform(int mode, int lutSize, const float* data, float rgb[3])
{
    if (mode == kModeMatrix) {
        const float* dec = data + kDecodeOffset;
        const float* enc = data + kEncodeOffset;
        const float* m = data + kMatrixOffset;
        const float r = tableLookup(dec, rgb[0]);
        const float g = tableLookup(dec, rgb[1]);
        const float b = tableLookup(dec, rgb[2]);
        const float r2 = std::fma(m[0], r, std::fma(m[1], g, m[2] * b));
        const float g2 = std::fma(m[3], r, std::fma(m[4], g, m[5] * b));
        const float b2 = std::fma(m[6], r, std::fma(m[7], g, m[8] * b));
        rgb[0] = tableLookup(enc, clamp01(r2));
        rgb[1] = tableLookup(enc, clamp01(g2));
        rgb[2] = tableLookup(enc, clamp01(b2));
    } else if (mode == kModeLut3D) {
        const int n = lutSize;
        const float top = static_cast<float>(n - 1);
        int idx[3];
        float frac[3];
        for (int c = 0; c < 3; ++c) {
            const float s = std::fmin(std::fmax(std::fma(rgb[c], data[kLutScaleOffset + c],
                                                         data[kLutBiasOffset + c]), 0.0f), top);
            int i = static_cast<int>(s);  // s >= 0: truncation is floor
            if (i > n - 2) i = n - 2;
            idx[c] = i;
            frac[c] = s - static_cast<float>(i);  // exact
        }
        const float* lut = data + kLutDataOffset;
        const int dr = 3, dg = n * 3, db = n * n * 3;
        const int base = ((idx[2] * n + idx[1]) * n + idx[0]) * 3;
        for (int ch = 0; ch < 3; ++ch) {
            const float* p = lut + base + ch;
            const float a00 = std::fma(p[dr] - p[0], frac[0], p[0]);
            const float a10 = std::fma(p[dg + dr] - p[dg], frac[0], p[dg]);
            const float a01 = std::fma(p[db + dr] - p[db], frac[0], p[db]);
            const float a11 = std::fma(p[db + dg + dr] - p[db + dg], frac[0], p[db + dg]);
            const float b0 = std::fma(a10 - a00, frac[1], a00);
            const float b1 = std::fma(a11 - a01, frac[1], a01);
            rgb[ch] = clamp01(std::fma(b1 - b0, frac[2], b0));
        }
    }
}

inline void applyTransform(const Transform& t, float rgb[3])
{
    if (t.mode != kModeNone) {
        applyTransform(t.mode, t.lutSize, t.data.data(), rgb);
    }
}

// --- the lsvr_color frame tag (issue #57 Part 4) ------------------------------------
// NDI's own ndi_color_info can't name P3-D65 or gamma 2.2/2.6 (SDK docs p. 39),
// so frames whose output space is known carry this per-frame XML instead,
// for receivers that read it (VR.NDI; ndi_verify prints it). The stream is
// always BT.709-matrix, limited-range Y'CbCr on the wire (src/UYVYEncode.h).

inline const char* colorTagXml(int space)
{
    static const char* const kTags[kDisplaySpaceCount] = {
        "<lsvr_color primaries=\"bt_709\" transfer=\"gamma_2.4\" matrix=\"bt_709\" range=\"limited\"/>",
        "<lsvr_color primaries=\"bt_709\" transfer=\"gamma_2.2\" matrix=\"bt_709\" range=\"limited\"/>",
        "<lsvr_color primaries=\"bt_709\" transfer=\"srgb\" matrix=\"bt_709\" range=\"limited\"/>",
        "<lsvr_color primaries=\"p3_d65\" transfer=\"gamma_2.2\" matrix=\"bt_709\" range=\"limited\"/>",
        "<lsvr_color primaries=\"p3_d65\" transfer=\"gamma_2.6\" matrix=\"bt_709\" range=\"limited\"/>",
        "<lsvr_color primaries=\"p3_d65\" transfer=\"srgb\" matrix=\"bt_709\" range=\"limited\"/>",
        "<lsvr_color primaries=\"bt_2020\" transfer=\"gamma_2.4\" matrix=\"bt_709\" range=\"limited\"/>",
    };
    return (space >= 0 && space < kDisplaySpaceCount) ? kTags[space] : nullptr;
}

namespace detail {

// Value of attribute `name` inside [begin, end), or "" when absent.
inline std::string tagAttribute(const char* begin, const char* end, const char* name)
{
    const std::string key = std::string(" ") + name + "=\"";
    const std::string text(begin, end);
    const size_t at = text.find(key);
    if (at == std::string::npos) return std::string();
    const size_t valueStart = at + key.size();
    const size_t close = text.find('"', valueStart);
    return close == std::string::npos ? std::string() : text.substr(valueStart, close - valueStart);
}

} // namespace detail

// Find an lsvr_color element in a frame's metadata and map it back to a
// display space. False when there is none or it names no known space.
inline bool parseColorTag(const char* xml, int* space)
{
    if (!xml) return false;
    const char* tag = std::strstr(xml, "<lsvr_color");
    if (!tag) return false;
    const char* end = std::strchr(tag, '>');
    if (!end) return false;
    const std::string prim = detail::tagAttribute(tag, end, "primaries");
    const std::string xfer = detail::tagAttribute(tag, end, "transfer");
    for (int s = 0; s < kDisplaySpaceCount; ++s) {
        const char* candidate = colorTagXml(s);
        const char* cEnd = candidate + std::strlen(candidate);
        if (detail::tagAttribute(candidate, cEnd, "primaries") == prim &&
            detail::tagAttribute(candidate, cEnd, "transfer") == xfer) {
            *space = s;
            return true;
        }
    }
    return false;
}

} // namespace ndi_color

#endif
