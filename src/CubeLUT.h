#ifndef _CubeLUT_h_
#define _CubeLUT_h_

/*
  .cube 3D LUT reader (issue #57 Part 2, NDI Output = LUT File).

  The faithful way to stream a log timeline (DaVinci Wide Gamut/Intermediate,
  ACEScct) as a display picture is a LUT Resolve itself generated — a Color
  Space Transform node with the project's tone/gamut mapping, exported with
  Generate LUT — so this reads the Adobe/Resolve .cube text format:

    TITLE "..."                      optional
    LUT_3D_SIZE N                    required, 2..65
    DOMAIN_MIN r g b / DOMAIN_MAX r g b
    LUT_3D_INPUT_RANGE min max       Resolve's spelling of a uniform domain
    N^3 lines "r g b", red varying fastest
    # comments, blank lines, CRLF

  Everything else is REJECTED with a message naming the line — 1D LUTs and
  shaper+3D files, unknown keywords, wrong value counts, NaN/inf, keywords
  after the data. A keyword we don't understand could change the meaning of
  the data (video-range flags, shapers), and a silently misread LUT streams a
  wrong picture that looks plausible; the plugin turns a rejection into
  Passthrough plus a Stream Status message instead.

  Numbers are parsed by hand, not strtod: strtod obeys the process locale, and
  a host running in a comma-decimal locale would read "0.5" as 0. Kept free of
  OFX/NDI/GPU dependencies (tests/test_cube_lut.cpp).
*/

#include "PlatformPaths.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ndi_cube {

constexpr int kMinSize = 2;
constexpr int kMaxSize = 65;
constexpr long long kMaxFileBytes = 64LL << 20;  // a 65-point cube is ~8 MB of text

struct CubeLUT {
    int size = 0;
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};
    std::string title;
    std::vector<float> rgb;  // size^3 triples, red fastest, then green, then blue
};

namespace detail {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Locale-independent decimal → float. Accepts [+-]digits[.digits][(e|E)[+-]digits]
// with at least one mantissa digit; the number must end at whitespace or the
// end of the token range. Up to 19 significant digits are kept exactly and
// scaled by an exact power of ten, so typical LUT values (6-10 digits) land
// on the correctly rounded float.
inline bool parseNumber(const char*& p, const char* end, float* out)
{
    const char* s = p;
    bool negative = false;
    if (s < end && (*s == '+' || *s == '-')) {
        negative = (*s == '-');
        ++s;
    }
    uint64_t mantissa = 0;
    int digits = 0;        // significant digits kept in mantissa
    int exp10 = 0;
    bool anyDigit = false;
    while (s < end && isDigit(*s)) {
        anyDigit = true;
        if (digits < 19) {
            if (mantissa != 0 || *s != '0') {
                mantissa = mantissa * 10 + static_cast<uint64_t>(*s - '0');
                ++digits;
            }
        } else {
            ++exp10;  // dropped integer digit still scales the value
        }
        ++s;
    }
    if (s < end && *s == '.') {
        ++s;
        while (s < end && isDigit(*s)) {
            anyDigit = true;
            if (digits < 19) {
                if (mantissa != 0 || *s != '0') {
                    mantissa = mantissa * 10 + static_cast<uint64_t>(*s - '0');
                    ++digits;
                }
                --exp10;
            }
            ++s;
        }
    }
    if (!anyDigit) {
        return false;
    }
    if (s < end && (*s == 'e' || *s == 'E')) {
        ++s;
        bool expNegative = false;
        if (s < end && (*s == '+' || *s == '-')) {
            expNegative = (*s == '-');
            ++s;
        }
        if (s >= end || !isDigit(*s)) {
            return false;
        }
        int e = 0;
        while (s < end && isDigit(*s)) {
            if (e < 100000) e = e * 10 + (*s - '0');
            ++s;
        }
        exp10 += expNegative ? -e : e;
    }
    if (s < end && !isSpace(*s)) {
        return false;  // "0.5.5", "0,5", "1x" ...
    }

    static const double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    double value = static_cast<double>(mantissa);
    if (mantissa != 0) {
        if (exp10 >= 0 && exp10 <= 22) {
            value *= kPow10[exp10];
        } else if (exp10 < 0 && exp10 >= -22) {
            value /= kPow10[-exp10];
        } else {
            value *= std::pow(10.0, static_cast<double>(exp10));
        }
    }
    const float f = static_cast<float>(negative ? -value : value);
    if (!std::isfinite(f)) {
        return false;
    }
    *out = f;
    p = s;
    return true;
}

// Splits [p, end) into whitespace-separated tokens.
inline void tokens(const char* p, const char* end, std::vector<std::pair<const char*, const char*>>* out)
{
    out->clear();
    while (p < end) {
        while (p < end && isSpace(*p)) ++p;
        const char* t = p;
        while (p < end && !isSpace(*p)) ++p;
        if (p > t) out->push_back({t, p});
    }
}

inline bool tokenIs(const std::pair<const char*, const char*>& t, const char* word)
{
    const size_t n = static_cast<size_t>(t.second - t.first);
    return n == std::strlen(word) && std::memcmp(t.first, word, n) == 0;
}

inline bool parseFloats(const std::vector<std::pair<const char*, const char*>>& tok, size_t first, size_t count,
                        float* out)
{
    if (tok.size() != first + count) return false;
    for (size_t i = 0; i < count; ++i) {
        const char* p = tok[first + i].first;
        if (!parseNumber(p, tok[first + i].second, &out[i]) || p != tok[first + i].second) return false;
    }
    return true;
}

inline bool fail(std::string* error, int line, const std::string& what)
{
    if (error) {
        *error = line > 0 ? "line " + std::to_string(line) + ": " + what : what;
    }
    return false;
}

} // namespace detail

// Parse .cube text. On failure returns false with a reason in *error and
// leaves *out unspecified.
inline bool parseCube(const char* text, size_t length, CubeLUT* out, std::string* error)
{
    using namespace detail;
    *out = CubeLUT();
    const char* p = text;
    const char* const end = text + length;
    int lineNo = 0;
    bool haveSize = false, haveDomainMin = false, haveDomainMax = false, haveRange = false, inData = false;
    size_t expected = 0;
    std::vector<std::pair<const char*, const char*>> tok;

    while (p < end) {
        const char* lineStart = p;
        while (p < end && *p != '\n') ++p;
        const char* lineEnd = p;
        if (p < end) ++p;  // past '\n'
        ++lineNo;

        // TITLE "..." may legitimately contain '#'; take the title text
        // before stripping comments. Seen in the LUTs Resolve ships: the
        // quoted form, Blackmagic's BMD_TITLE "...", and Sony's unquoted
        // TITLE <text>. An opening quote without a closing one is malformed.
        const char* q = lineStart;
        while (q < lineEnd && isSpace(*q)) ++q;
        size_t titleLen = 0;
        if (lineEnd - q >= 5 && std::memcmp(q, "TITLE", 5) == 0) titleLen = 5;
        if (lineEnd - q >= 9 && std::memcmp(q, "BMD_TITLE", 9) == 0) titleLen = 9;
        if (titleLen && (q + titleLen == lineEnd || isSpace(q[titleLen]))) {
            if (inData) return fail(error, lineNo, "TITLE after the LUT data started");
            const char* t = q + titleLen;
            while (t < lineEnd && isSpace(*t)) ++t;
            const char* tEnd = lineEnd;
            while (tEnd > t && isSpace(tEnd[-1])) --tEnd;
            if (t < tEnd && *t == '"') {
                const char* close = static_cast<const char*>(std::memchr(t + 1, '"', static_cast<size_t>(tEnd - t - 1)));
                if (!close) return fail(error, lineNo, "TITLE has an unterminated quote");
                out->title.assign(t + 1, close);
            } else {
                out->title.assign(t, tEnd);
            }
            continue;
        }

        const char* hash = static_cast<const char*>(std::memchr(lineStart, '#', static_cast<size_t>(lineEnd - lineStart)));
        tokens(lineStart, hash ? hash : lineEnd, &tok);
        if (tok.empty()) continue;

        const char c0 = *tok[0].first;
        const bool isData = isDigit(c0) || c0 == '-' || c0 == '+' || c0 == '.';
        if (isData) {
            if (!haveSize) {
                return fail(error, lineNo, "LUT data before LUT_3D_SIZE (only 3D .cube LUTs are supported)");
            }
            inData = true;
            if (tok.size() != 3) {
                return fail(error, lineNo, "expected 3 numbers, found " + std::to_string(tok.size()));
            }
            if (out->rgb.size() >= expected * 3) {
                return fail(error, lineNo, "more than the " + std::to_string(expected) +
                                               " data lines LUT_3D_SIZE " + std::to_string(out->size) + " calls for");
            }
            float v[3];
            if (!parseFloats(tok, 0, 3, v)) {
                return fail(error, lineNo, "not three finite numbers (decimal point must be '.')");
            }
            out->rgb.insert(out->rgb.end(), v, v + 3);
            continue;
        }

        const std::string keyword(tok[0].first, tok[0].second);
        if (inData) return fail(error, lineNo, "keyword " + keyword + " after the LUT data started");
        if (keyword == "LUT_3D_SIZE") {
            if (haveSize) return fail(error, lineNo, "duplicate LUT_3D_SIZE");
            float n = 0.0f;
            if (!parseFloats(tok, 1, 1, &n) || n != std::floor(n)) {
                return fail(error, lineNo, "LUT_3D_SIZE needs one whole number");
            }
            if (n < kMinSize || n > kMaxSize) {
                return fail(error, lineNo, "LUT_3D_SIZE " + std::to_string(static_cast<long long>(n)) +
                                               " is outside 2..65");
            }
            out->size = static_cast<int>(n);
            expected = static_cast<size_t>(out->size) * out->size * out->size;
            out->rgb.reserve(expected * 3);
            haveSize = true;
        } else if (keyword == "LUT_1D_SIZE" || keyword == "LUT_1D_INPUT_RANGE") {
            return fail(error, lineNo, "1D LUTs (and 1D shaper + 3D files) are not supported — export a 3D .cube");
        } else if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
            const bool isMin = (keyword == "DOMAIN_MIN");
            if ((isMin && haveDomainMin) || (!isMin && haveDomainMax)) {
                return fail(error, lineNo, "duplicate " + keyword);
            }
            if (!parseFloats(tok, 1, 3, isMin ? out->domainMin : out->domainMax)) {
                return fail(error, lineNo, keyword + " needs three numbers");
            }
            (isMin ? haveDomainMin : haveDomainMax) = true;
        } else if (keyword == "LUT_3D_INPUT_RANGE") {
            float range[2];
            if (haveRange || !parseFloats(tok, 1, 2, range)) {
                return fail(error, lineNo, "LUT_3D_INPUT_RANGE needs two numbers (once)");
            }
            for (int c = 0; c < 3; ++c) {
                out->domainMin[c] = range[0];
                out->domainMax[c] = range[1];
            }
            haveRange = true;
        } else {
            return fail(error, lineNo, "unsupported keyword " + keyword);
        }
    }

    if (!haveSize) {
        return fail(error, 0, "no LUT_3D_SIZE line (only 3D .cube LUTs are supported)");
    }
    if (haveRange && (haveDomainMin || haveDomainMax)) {
        return fail(error, 0, "both LUT_3D_INPUT_RANGE and DOMAIN_MIN/MAX are set");
    }
    for (int c = 0; c < 3; ++c) {
        if (!(out->domainMax[c] > out->domainMin[c])) {
            return fail(error, 0, "DOMAIN_MAX must exceed DOMAIN_MIN on every channel");
        }
    }
    if (out->rgb.size() != expected * 3) {
        return fail(error, 0, "found " + std::to_string(out->rgb.size() / 3) + " data lines, LUT_3D_SIZE " +
                                  std::to_string(out->size) + " needs " + std::to_string(expected));
    }
    return true;
}

// Read and parse a .cube file (UTF-8 path on every platform).
inline bool loadCubeFile(const char* path, CubeLUT* out, std::string* error)
{
    long long mtime = 0, size = 0;
    if (!path || !*path || !ndi_path::statUtf8(path, &mtime, &size)) {
        return detail::fail(error, 0, std::string("cannot open '") + (path ? path : "") + "'");
    }
    if (size <= 0 || size > kMaxFileBytes) {
        return detail::fail(error, 0, "file is empty or larger than 64 MB");
    }
    std::FILE* f = ndi_path::fopenUtf8(path, "rb");
    if (!f) {
        return detail::fail(error, 0, std::string("cannot open '") + path + "'");
    }
    std::string text(static_cast<size_t>(size), '\0');
    const size_t got = std::fread(&text[0], 1, text.size(), f);
    std::fclose(f);
    if (got != text.size()) {
        return detail::fail(error, 0, "short read");
    }
    return parseCube(text.data(), text.size(), out, error);
}

} // namespace ndi_cube

#endif
