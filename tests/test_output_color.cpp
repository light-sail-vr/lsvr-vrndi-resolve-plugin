// Tests for the output-colour seam (src/OutputColor.h, issue #57 Part 2):
// the display-to-display presets (transfer tables, 3x3 primaries matrices,
// clipping) and the 3D LUT path, which the CPU converter runs and the
// Metal/CUDA kernels must reproduce byte-for-byte.
//
// Oracles, none of them the code under test:
//   - published conversion matrices (ITU-R BT.2087 for 709 -> 2020; the
//     standard 709 -> P3-D65 matrix) and published sRGB curve values;
//   - a double-precision reference built here from first principles
//     (chromaticities -> RGB<->XYZ, pow-based curves), swept over a 33^3 cube
//     plus a 4096-step grey ramp for every preset pair;
//   - the mathematical fact that trilinear interpolation reproduces an
//     affine function exactly, and that a 2^k+1 identity lattice is exact.
// Build & run: make test

#include "OutputColor.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

static void expect(bool ok, const char* name)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

static bool near(float a, double b, double tol) { return std::fabs(static_cast<double>(a) - b) <= tol; }

// --- double-precision reference ----------------------------------------------

struct Prim { double rx, ry, gx, gy, bx, by; };
static const double kWx = 0.3127, kWy = 0.3290;  // D65
static const Prim kBT709 = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
static const Prim kP3D65 = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060};
static const Prim kBT2020 = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};

static void invert3(const double m[9], double out[9])
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

static void rgbToXyz(const Prim& p, double m[9])
{
    const double xs[3] = {p.rx, p.gx, p.bx}, ys[3] = {p.ry, p.gy, p.by};
    double P[9];
    for (int c = 0; c < 3; ++c) {
        P[0 * 3 + c] = xs[c] / ys[c];
        P[1 * 3 + c] = 1.0;
        P[2 * 3 + c] = (1.0 - xs[c] - ys[c]) / ys[c];
    }
    double Pi[9];
    invert3(P, Pi);
    const double W[3] = {kWx / kWy, 1.0, (1.0 - kWx - kWy) / kWy};
    double S[3];
    for (int r = 0; r < 3; ++r) S[r] = Pi[r * 3] * W[0] + Pi[r * 3 + 1] * W[1] + Pi[r * 3 + 2] * W[2];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r * 3 + c] = P[r * 3 + c] * S[c];
}

static const Prim& primOf(int space)
{
    switch (space) {
        case ndi_color::kP3D65Gamma22:
        case ndi_color::kP3D65Gamma26:
        case ndi_color::kDisplayP3: return kP3D65;
        case ndi_color::kRec2020Gamma24: return kBT2020;
        default: return kBT709;
    }
}

static double refDecode(int space, double v)
{
    switch (space) {
        case ndi_color::kRec709Gamma22:
        case ndi_color::kP3D65Gamma22: return std::pow(v, 2.2);
        case ndi_color::kP3D65Gamma26: return std::pow(v, 2.6);
        case ndi_color::kSRGB:
        case ndi_color::kDisplayP3: return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        default: return std::pow(v, 2.4);
    }
}

static double refEncode(int space, double l)
{
    switch (space) {
        case ndi_color::kRec709Gamma22:
        case ndi_color::kP3D65Gamma22: return std::pow(l, 1.0 / 2.2);
        case ndi_color::kP3D65Gamma26: return std::pow(l, 1.0 / 2.6);
        case ndi_color::kSRGB:
        case ndi_color::kDisplayP3: return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
        default: return std::pow(l, 1.0 / 2.4);
    }
}

static void refConvert(int in, int out, const double rgb[3], double res[3])
{
    double toXyz[9], fromXyz[9], xyzOut[9];
    rgbToXyz(primOf(in), toXyz);
    rgbToXyz(primOf(out), xyzOut);
    invert3(xyzOut, fromXyz);
    double lin[3], xyz[3];
    for (int c = 0; c < 3; ++c) lin[c] = refDecode(in, rgb[c]);
    for (int r = 0; r < 3; ++r) xyz[r] = toXyz[r * 3] * lin[0] + toXyz[r * 3 + 1] * lin[1] + toXyz[r * 3 + 2] * lin[2];
    for (int r = 0; r < 3; ++r) {
        double v = fromXyz[r * 3] * xyz[0] + fromXyz[r * 3 + 1] * xyz[1] + fromXyz[r * 3 + 2] * xyz[2];
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        res[r] = refEncode(out, v);
    }
}

static void apply(const ndi_color::Transform& t, float r, float g, float b, float out[3])
{
    out[0] = r;
    out[1] = g;
    out[2] = b;
    ndi_color::applyTransform(t, out);
}

static ndi_cube::CubeLUT latticeLut(int n, float dmin, float dmax, void (*f)(double, double, double, double*))
{
    ndi_cube::CubeLUT lut;
    lut.size = n;
    for (int c = 0; c < 3; ++c) {
        lut.domainMin[c] = dmin;
        lut.domainMax[c] = dmax;
    }
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const double step = (static_cast<double>(dmax) - dmin) / (n - 1);
                double v[3];
                f(dmin + r * step, dmin + g * step, dmin + b * step, v);
                lut.rgb.push_back(static_cast<float>(v[0]));
                lut.rgb.push_back(static_cast<float>(v[1]));
                lut.rgb.push_back(static_cast<float>(v[2]));
            }
    return lut;
}

static void identityFn(double r, double g, double b, double* v) { v[0] = r; v[1] = g; v[2] = b; }
static void affineFn(double r, double g, double b, double* v)
{
    v[0] = 0.5 * r + 0.25 * g;
    v[1] = 1.0 - b;
    v[2] = 0.1 + 0.8 * g;
}

int main()
{
    using namespace ndi_color;

    // --- identity presets are no-ops ------------------------------------------
    {
        bool allNone = true;
        for (int s = 0; s < kDisplaySpaceCount; ++s) {
            Transform t;
            allNone = allNone && buildPreset(s, s, &t) && t.mode == kModeNone;
        }
        expect(allNone, "same in/out space builds no transform (stream stays untouched)");
        Transform bad;
        expect(!buildPreset(-1, 0, &bad) && !buildPreset(0, kDisplaySpaceCount, &bad),
               "out-of-range spaces are refused");
    }

    // --- published matrices --------------------------------------------------
    {
        // BT.2087: Rec.709 -> Rec.2020 linear, applied to pure 709 red, green.
        Transform t;
        buildPreset(kRec709Gamma24, kRec2020Gamma24, &t);
        float o[3];
        apply(t, 1.0f, 0.0f, 0.0f, o);
        const double tol = 0.0006;  // the published matrix is rounded to 4 decimals
        expect(near(o[0], std::pow(0.6274, 1 / 2.4), tol) && near(o[1], std::pow(0.0691, 1 / 2.4), tol) &&
                   near(o[2], std::pow(0.0164, 1 / 2.4), tol),
               "709 red -> 2020 matches BT.2087 (0.6274, 0.0691, 0.0164)");
        apply(t, 0.0f, 1.0f, 0.0f, o);
        expect(near(o[0], std::pow(0.3293, 1 / 2.4), tol) && near(o[1], std::pow(0.9195, 1 / 2.4), tol) &&
                   near(o[2], std::pow(0.0880, 1 / 2.4), tol),
               "709 green -> 2020 matches BT.2087 (0.3293, 0.9195, 0.0880)");

        Transform p3;
        buildPreset(kRec709Gamma24, kP3D65Gamma22, &p3);
        apply(p3, 1.0f, 0.0f, 0.0f, o);
        expect(near(o[0], std::pow(0.8225, 1 / 2.2), tol) && near(o[1], std::pow(0.0332, 1 / 2.2), tol) &&
                   near(o[2], std::pow(0.0171, 1 / 2.2), tol),
               "709 red -> P3-D65 matches (0.8225, 0.0332, 0.0171)");
    }

    // --- published curve values ---------------------------------------------
    {
        // sRGB 0.5 decodes to linear 0.2140411; re-encoded as pure gamma 2.4.
        Transform t;
        buildPreset(kSRGB, kRec709Gamma24, &t);
        float o[3];
        apply(t, 0.5f, 0.5f, 0.5f, o);
        expect(near(o[0], std::pow(0.2140411, 1 / 2.4), 2e-6) && o[0] == o[1] && o[1] == o[2],
               "sRGB 0.5 -> linear 0.2140411 -> G2.4, neutral stays neutral");
        // White and black are fixed points of every preset (shared D65 white).
        bool fixed = true;
        for (int a = 0; a < kDisplaySpaceCount; ++a)
            for (int b = 0; b < kDisplaySpaceCount; ++b) {
                Transform p;
                buildPreset(a, b, &p);
                apply(p, 1.0f, 1.0f, 1.0f, o);
                fixed = fixed && near(o[0], 1.0, 2e-6) && near(o[1], 1.0, 2e-6) && near(o[2], 1.0, 2e-6);
                apply(p, 0.0f, 0.0f, 0.0f, o);
                fixed = fixed && o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f;
            }
        expect(fixed, "black and white are fixed points of every preset pair");
    }

    // --- out-of-gamut clips ----------------------------------------------------
    {
        Transform t;
        buildPreset(kRec2020Gamma24, kRec709Gamma24, &t);
        float o[3];
        apply(t, 0.0f, 1.0f, 0.0f, o);  // 2020 green is far outside 709
        expect(o[0] == 0.0f && o[1] == 1.0f && o[2] == 0.0f, "2020 green clips to 709 green (no gamut mapping)");
    }

    // --- every preset pair vs. the double reference ----------------------------
    {
        double worst = 0.0;
        int pairs = 0;
        for (int a = 0; a < kDisplaySpaceCount; ++a) {
            for (int b = 0; b < kDisplaySpaceCount; ++b) {
                if (a == b) continue;
                Transform t;
                buildPreset(a, b, &t);
                ++pairs;
                for (int i = 0; i < 33 * 33 * 33 + 4096; ++i) {
                    double in[3];
                    if (i < 33 * 33 * 33) {
                        in[0] = (i % 33) / 32.0;
                        in[1] = (i / 33 % 33) / 32.0;
                        in[2] = (i / 1089) / 32.0;
                    } else {
                        in[0] = in[1] = in[2] = (i - 33 * 33 * 33) / 4095.0;
                    }
                    float o[3];
                    apply(t, static_cast<float>(in[0]), static_cast<float>(in[1]), static_cast<float>(in[2]), o);
                    const double fin[3] = {static_cast<float>(in[0]), static_cast<float>(in[1]),
                                           static_cast<float>(in[2])};
                    double ref[3];
                    refConvert(a, b, fin, ref);
                    for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(o[c] - ref[c]) * 255.0);
                }
            }
        }
        std::printf("     preset sweep: %d pairs, worst |float - double| = %.4f code\n", pairs, worst);
        expect(pairs == 42 && worst < 0.5, "all 42 preset pairs within half a code of the double reference");
    }

    // --- 3D LUTs ---------------------------------------------------------------
    {
        // Identity lattices with 2^k+1 points are exact: same floats in, same
        // floats out, so an identity LUT streams the same bytes as Passthrough.
        bool exact = true;
        for (int n : {2, 17, 33, 65}) {
            Transform t;
            const bool built = buildLut(latticeLut(n, 0.0f, 1.0f, identityFn), &t) && t.mode == kModeLut3D;
            exact = exact && built;
            for (int i = 0; i < 20000 && built; ++i) {
                const float v[3] = {(i % 251) / 250.0f, (i * 7 % 1021) / 1020.0f, (i * 13 % 4093) / 4092.0f};
                float o[3];
                apply(t, v[0], v[1], v[2], o);
                exact = exact && std::memcmp(o, v, sizeof(o)) == 0;
            }
        }
        expect(exact, "identity LUTs of 2/17/33/65 points reproduce the input bit-for-bit");

        // Trilinear interpolation reproduces any affine function exactly
        // (up to float rounding), anywhere inside the lattice.
        Transform t;
        buildLut(latticeLut(9, 0.0f, 1.0f, affineFn), &t);
        double worst = 0.0;
        for (int i = 0; i < 5000; ++i) {
            const double r = (i % 97) / 96.0, g = (i * 31 % 89) / 88.0, b = (i * 17 % 83) / 82.0;
            float o[3];
            apply(t, static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), o);
            double e[3];
            affineFn(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), e);
            for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(o[c] - e[c]));
        }
        expect(worst < 2e-6, "trilinear reproduces an affine LUT between lattice points");

        // DOMAIN_MIN/MAX: a lattice over [-0.5, 1.5] holding its own
        // coordinates maps x to x.
        Transform wide;
        buildLut(latticeLut(33, -0.5f, 1.5f, identityFn), &wide);
        float o[3];
        apply(wide, 0.25f, 0.5f, 0.75f, o);
        expect(near(o[0], 0.25, 1e-6) && near(o[1], 0.5, 1e-6) && near(o[2], 0.75, 1e-6),
               "DOMAIN_MIN/MAX rescale the input onto the lattice");

        // Output is clipped to [0,1] (an affine LUT can exceed it).
        Transform over;
        buildLut(latticeLut(2, 0.0f, 1.0f, [](double r, double g, double b, double* v) {
                     v[0] = 2.0 * r - 0.5;
                     v[1] = g;
                     v[2] = b;
                 }),
                 &over);
        apply(over, 1.0f, 0.5f, 0.0f, o);
        float o2[3];
        apply(over, 0.0f, 0.5f, 0.0f, o2);
        expect(o[0] == 1.0f && o2[0] == 0.0f, "LUT output is clipped to [0,1]");

        Transform bad;
        ndi_cube::CubeLUT empty;
        expect(!buildLut(empty, &bad), "an unparsed (size 0) LUT is refused");
    }

    // --- hostile input never escapes [0,1] or reads out of bounds ---------------
    {
        bool sane = true;
        const float nanv = std::nanf("");
        const float hostile[] = {nanv, -0.0f, 1e-30f, 1e-40f, 1.0f};
        Transform preset, lut;
        buildPreset(kRec709Gamma24, kDisplayP3, &preset);
        buildLut(latticeLut(17, 0.0f, 1.0f, identityFn), &lut);
        for (const Transform* t : {&preset, &lut})
            for (float v : hostile) {
                float o[3];
                apply(*t, v, v, v, o);
                for (float x : o) sane = sane && x >= 0.0f && x <= 1.0f;
            }
        expect(sane, "NaN, -0, tiny and denormal inputs stay finite and in [0,1]");
    }

    // --- the lsvr_color frame tag (Part 4) ------------------------------------------
    {
        expect(std::string(colorTagXml(kRec709Gamma24)) ==
                   "<lsvr_color primaries=\"bt_709\" transfer=\"gamma_2.4\" matrix=\"bt_709\" range=\"limited\"/>",
               "Rec.709 G2.4 tag spelled as issue #57 specifies");
        expect(std::string(colorTagXml(kDisplayP3)) ==
                   "<lsvr_color primaries=\"p3_d65\" transfer=\"srgb\" matrix=\"bt_709\" range=\"limited\"/>",
               "Display P3 tag: P3-D65 primaries, sRGB curve, BT.709 matrix");
        expect(std::string(colorTagXml(kRec2020Gamma24)).find("primaries=\"bt_2020\"") != std::string::npos &&
                   std::string(colorTagXml(kP3D65Gamma26)).find("transfer=\"gamma_2.6\"") != std::string::npos,
               "Rec.2020 and P3-D65 G2.6 tags");
        expect(colorTagXml(-1) == nullptr && colorTagXml(kDisplaySpaceCount) == nullptr,
               "no tag for an unknown output space");
        bool roundTrip = true;
        for (int sp = 0; sp < kDisplaySpaceCount; ++sp) {
            int parsed = -1;
            roundTrip = roundTrip && parseColorTag(colorTagXml(sp), &parsed) && parsed == sp;
        }
        expect(roundTrip, "every tag parses back to its space");
        int parsed = -1;
        expect(parseColorTag("<ndi_codec x=\"1\"/><lsvr_color range=\"limited\" transfer=\"gamma_2.2\" "
                             "primaries=\"p3_d65\" matrix=\"bt_709\"/>",
                             &parsed) &&
                   parsed == kP3D65Gamma22,
               "the tag is found among other metadata, attributes in any order");
        expect(!parseColorTag(nullptr, &parsed) && !parseColorTag("", &parsed) &&
                   !parseColorTag("<lsvr_color primaries=\"bt_709\"/>", &parsed) &&
                   !parseColorTag("<lsvr_color primaries=\"aces\" transfer=\"gamma_2.4\"/>", &parsed),
               "missing, partial or unknown tags are not parsed");
    }

    // --- names --------------------------------------------------------------------
    expect(std::string(displaySpaceName(kP3D65Gamma22)) == "P3-D65 Gamma 2.2" &&
               std::string(displaySpaceShortName(kRec709Gamma24)) == "Rec.709 G2.4",
           "display-space names for the UI and Stream Status");

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All output-colour tests passed\n");
    return 0;
}
