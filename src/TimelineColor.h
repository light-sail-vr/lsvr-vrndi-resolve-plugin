#ifndef _TimelineColor_h_
#define _TimelineColor_h_

/*
  Timeline colour space detection for Timeline Color = Auto (issue #57
  Part 3). An OpenFX node on the Color page runs in Resolve's TIMELINE colour
  space (manual p. 3407: the Output LUT and the RCM/ACES output transform run
  after every node), so a preset conversion needs to know what that space is.
  Two sources, in the plugin's order of preference:

  1. The OFX 1.5 colour-management property kOfxImageClipPropColourspace on
     the source clip, when the host reports one (identifiers from
     ofx-native-v1.5_aces-v1.3_ocio-v2.3.h) — fromOfxColourspace().
  2. Resolve's timeline colour settings, read by the timeline-watch helper
     (src/ndi_timeline_watch.lua / .py) and sent as one protocol line:
       @color<TAB>colorScienceMode<TAB>separateColorSpaceAndGamma<TAB>
              colorSpaceTimeline<TAB>colorSpaceTimelineGamma<TAB>
              colorSpaceOutput<TAB>colorSpaceOutputGamma
     — parseColorLine() + timelineFromSettings().

  Each resolves to a display space (an ndi_color::DisplaySpace the presets
  can convert from), scene-referred/log (only a LUT can turn that into a
  display picture), HDR (out of scope) or unknown — with Resolve's own name
  kept as the label for Stream Status.

  Resolve 21.1 quirks this encodes (probed 2026-10-19, LEARNINGS.md):
  - With separateColorSpaceAndGamma = 0 the gamut field holds a combined
    name ("Rec.709 Gamma 2.4", "P3-D65", "DaVinci WG/Intermediate") and the
    gamma field still reports whatever split-mode value it last had — so the
    gamma field is ignored in combined mode.
  - Combined "P3-D65" is P3-D65 gamma 2.6.
  - Under ACES (colorScienceMode acescct/acescc) the gamut/gamma fields are
    stale RCM values; the timeline is ACEScct/ACEScc regardless.
  - Unmanaged DaVinci YRGB has no output conversion and labels its timeline
    "Rec.709 (Scene)" by default; the pixels are graded for a BT.1886 display,
    so "(Scene)" there means Rec.709/Rec.2020 gamma 2.4. Under RCM the same
    name really is scene-referred.

  Kept free of OFX/NDI/host dependencies (tests/test_timeline_color.cpp).
*/

#include "OutputColor.h"

#include <string>
#include <vector>

namespace ndi_tlcolor {

using ndi_color::kDisplayP3;
using ndi_color::kP3D65Gamma22;
using ndi_color::kP3D65Gamma26;
using ndi_color::kRec2020Gamma24;
using ndi_color::kRec709Gamma22;
using ndi_color::kRec709Gamma24;
using ndi_color::kSRGB;

struct Settings {
    std::string scienceMode;    // colorScienceMode, e.g. davinciYRGBColorManagedv2
    std::string separateGamma;  // separateColorSpaceAndGamma, "0" or "1"
    std::string timelineSpace;  // colorSpaceTimeline
    std::string timelineGamma;  // colorSpaceTimelineGamma
    std::string outputSpace;    // colorSpaceOutput
    std::string outputGamma;    // colorSpaceOutputGamma
    bool operator==(const Settings& o) const
    {
        return scienceMode == o.scienceMode && separateGamma == o.separateGamma &&
               timelineSpace == o.timelineSpace && timelineGamma == o.timelineGamma &&
               outputSpace == o.outputSpace && outputGamma == o.outputGamma;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

enum Kind { kUnknown = 0, kDisplay, kSceneReferred, kHdr };

struct Resolved {
    Kind kind = kUnknown;
    int space = -1;      // ndi_color::DisplaySpace when kind == kDisplay
    std::string label;   // the host's own name, for Stream Status and logs
};

constexpr const char* kColorLineTag = "@color";

// The helper's colour line → fields. False for anything else (clip paths,
// '!' status lines, other '@' tags, short lines).
inline bool parseColorLine(const std::string& line, Settings* out)
{
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        f.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    if (f.size() != 7 || f[0] != kColorLineTag) {
        return false;
    }
    out->scienceMode = f[1];
    out->separateGamma = f[2];
    out->timelineSpace = f[3];
    out->timelineGamma = f[4];
    out->outputSpace = f[5];
    out->outputGamma = f[6];
    return true;
}

inline std::string formatColorLine(const Settings& s)
{
    return std::string(kColorLineTag) + "\t" + s.scienceMode + "\t" + s.separateGamma + "\t" + s.timelineSpace +
           "\t" + s.timelineGamma + "\t" + s.outputSpace + "\t" + s.outputGamma;
}

namespace detail {

inline bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

inline Resolved make(Kind kind, int space, const std::string& label)
{
    Resolved r;
    r.kind = kind;
    r.space = space;
    r.label = label;
    return r;
}

inline bool isHdrName(const std::string& s) { return has(s, "ST2084") || has(s, "HLG") || has(s, "2100"); }

inline bool isSceneName(const std::string& s)
{
    return has(s, "(Scene)") || has(s, "Intermediate") || has(s, "Linear") || has(s, "ACES") || has(s, "Log") ||
           has(s, "LOG") || has(s, "log") || has(s, "Film") || has(s, "Wide Gamut") || has(s, "WG");
}

// One colour space as Resolve names it: a combined name, or gamut + gamma.
inline Resolved resolveName(const std::string& space, const std::string& gamma, bool separate, bool unmanaged)
{
    if (!separate) {
        if (isHdrName(space)) return make(kHdr, -1, space);
        if (space == "Rec.709 Gamma 2.4") return make(kDisplay, kRec709Gamma24, space);
        if (space == "Rec.709 Gamma 2.2") return make(kDisplay, kRec709Gamma22, space);
        if (space == "sRGB") return make(kDisplay, kSRGB, space);
        if (space == "P3-D65") return make(kDisplay, kP3D65Gamma26, space);
        if (space == "Rec.2020 Gamma 2.4") return make(kDisplay, kRec2020Gamma24, space);
        if (unmanaged && space == "Rec.709 (Scene)") return make(kDisplay, kRec709Gamma24, space);
        if (unmanaged && space == "Rec.2020 (Scene)") return make(kDisplay, kRec2020Gamma24, space);
        if (isSceneName(space)) return make(kSceneReferred, -1, space);
        return make(kUnknown, -1, space);
    }

    const std::string label = space + " / " + gamma;
    if (isHdrName(gamma) || isHdrName(space)) return make(kHdr, -1, label);
    int prim = -1;  // ndi_color::Primaries
    if (space == "Rec.709" || space == "sRGB") prim = ndi_color::kPrimBT709;
    else if (space == "P3-D65") prim = ndi_color::kPrimP3D65;
    else if (space == "Rec.2020") prim = ndi_color::kPrimBT2020;

    int curve = -1;  // ndi_color::Curve
    if (gamma == "Gamma 2.4") curve = ndi_color::kCurveGamma24;
    else if (gamma == "Gamma 2.2") curve = ndi_color::kCurveGamma22;
    else if (gamma == "Gamma 2.6") curve = ndi_color::kCurveGamma26;
    else if (gamma == "sRGB") curve = ndi_color::kCurveSRGB;
    else if (unmanaged && gamma == "Rec.709") curve = ndi_color::kCurveGamma24;  // see the header note
    else if (gamma == "Rec.709" || isSceneName(gamma) || isSceneName(space)) return make(kSceneReferred, -1, label);

    if (prim == ndi_color::kPrimBT709) {
        if (curve == ndi_color::kCurveGamma24) return make(kDisplay, kRec709Gamma24, label);
        if (curve == ndi_color::kCurveGamma22) return make(kDisplay, kRec709Gamma22, label);
        if (curve == ndi_color::kCurveSRGB) return make(kDisplay, kSRGB, label);
    } else if (prim == ndi_color::kPrimP3D65) {
        if (curve == ndi_color::kCurveGamma22) return make(kDisplay, kP3D65Gamma22, label);
        if (curve == ndi_color::kCurveGamma26) return make(kDisplay, kP3D65Gamma26, label);
        if (curve == ndi_color::kCurveSRGB) return make(kDisplay, kDisplayP3, label);
    } else if (prim == ndi_color::kPrimBT2020) {
        if (curve == ndi_color::kCurveGamma24) return make(kDisplay, kRec2020Gamma24, label);
    }
    return make(kUnknown, -1, label);
}

inline bool isAces(const Settings& s) { return s.scienceMode.compare(0, 4, "aces") == 0; }

} // namespace detail

// The colour space an OpenFX node sees: Resolve's timeline space.
inline Resolved timelineFromSettings(const Settings& s)
{
    if (detail::isAces(s)) {
        return detail::make(kSceneReferred, -1, s.scienceMode == "acescc" ? "ACEScc" : "ACEScct");
    }
    return detail::resolveName(s.timelineSpace, s.timelineGamma, s.separateGamma == "1",
                               s.scienceMode == "davinciYRGB");
}

// Resolve's own output space (what its viewer/deliverables carry) — only a
// hint for Stream Status: the node never sees it.
inline Resolved outputFromSettings(const Settings& s)
{
    if (detail::isAces(s)) {
        return detail::make(kUnknown, -1, "ACES output transform");
    }
    return detail::resolveName(s.outputSpace, s.outputGamma, s.separateGamma == "1",
                               s.scienceMode == "davinciYRGB");
}

// An OFX 1.5 native colourspace identifier (kOfxImageClipPropColourspace).
inline Resolved fromOfxColourspace(const std::string& id)
{
    using detail::make;
    if (id == "rec1886_rec709_display" || id == "g24_rec709_tx") return make(kDisplay, kRec709Gamma24, id);
    if (id == "g22_rec709_tx") return make(kDisplay, kRec709Gamma22, id);
    if (id == "srgb_display" || id == "srgb_tx") return make(kDisplay, kSRGB, id);
    if (id == "displayp3_display" || id == "srgb_encoded_p3d65_tx") return make(kDisplay, kDisplayP3, id);
    if (id == "p3d65_display") return make(kDisplay, kP3D65Gamma26, id);
    if (id == "rec1886_rec2020_display") return make(kDisplay, kRec2020Gamma24, id);
    if (id == "ofx_display_hdr" || id.compare(0, 7, "rec2100") == 0 || id.compare(0, 6, "st2084") == 0) {
        return make(kHdr, -1, id);
    }
    static const char* const kScene[] = {
        "ofx_scene_linear", "ofx_scene_log", "ACES2065-1", "ACEScc", "ACEScct", "ACEScg",
        "davinci_intermediate_widegamut", "bmdfilm_widegamut_gen5", "scene_linear", "compositing_log",
        "color_timing", "camera_rec709", "ADX10", "ADX16", "CIE-XYZ-D65", "aces_interchange",
        "cie_xyz_d65_interchange"};
    for (const char* s : kScene) {
        if (id == s) return make(kSceneReferred, -1, id);
    }
    if (id.compare(0, 4, "lin_") == 0 || detail::has(id, "log")) return make(kSceneReferred, -1, id);
    return make(kUnknown, -1, id);
}

} // namespace ndi_tlcolor

#endif
