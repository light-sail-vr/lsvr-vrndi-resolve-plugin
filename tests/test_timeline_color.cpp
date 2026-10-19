// Tests for the timeline-colour seam (src/TimelineColor.h, issue #57 Part 3):
// the watcher helper's "@color" protocol line, and how Resolve's colour
// settings and OFX 1.5 colourspace identifiers resolve to one of the Output
// Color display spaces — or to "scene-referred/log", "HDR" or "unknown".
// The setting strings are the ones Resolve 21.1's scripting API returned on
// 2026-10-19 (probe in LEARNINGS.md), including its stale-field quirks.
// Build & run: make test

#include "TimelineColor.h"

#include <cstdio>
#include <string>

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

using namespace ndi_tlcolor;

static Settings settings(const char* mode, const char* separate, const char* tl, const char* tlGamma,
                         const char* out = "Rec.709 Gamma 2.4", const char* outGamma = "")
{
    Settings s;
    s.scienceMode = mode;
    s.separateGamma = separate;
    s.timelineSpace = tl;
    s.timelineGamma = tlGamma;
    s.outputSpace = out;
    s.outputGamma = outGamma;
    return s;
}

static bool isDisplay(const Resolved& r, int space) { return r.kind == kDisplay && r.space == space; }

int main()
{
    // --- the protocol line ------------------------------------------------------
    {
        Settings s;
        const bool ok = parseColorLine(
            "@color\tdavinciYRGBColorManagedv2\t0\tDaVinci WG/Intermediate\tST2084 1000 nit\tRec.709 Gamma 2.4\t", &s);
        expect(ok && s.scienceMode == "davinciYRGBColorManagedv2" && s.separateGamma == "0" &&
                   s.timelineSpace == "DaVinci WG/Intermediate" && s.timelineGamma == "ST2084 1000 nit" &&
                   s.outputSpace == "Rec.709 Gamma 2.4" && s.outputGamma.empty(),
               "a full @color line parses field by field (empty trailing field kept)");
        expect(formatColorLine(s) ==
                   "@color\tdavinciYRGBColorManagedv2\t0\tDaVinci WG/Intermediate\tST2084 1000 nit\tRec.709 Gamma 2.4\t",
               "formatColorLine round-trips the helper's format");
        Settings junk;
        expect(!parseColorLine("/Volumes/media/A001.braw", &junk), "a clip path is not a colour line");
        expect(!parseColorLine("!status", &junk), "a status line is not a colour line");
        expect(!parseColorLine("@color\tdavinciYRGB\t0", &junk), "a short @color line is rejected");
        expect(!parseColorLine("@colour\ta\tb\tc\td\te\tf", &junk), "another @ tag is not a colour line");
    }

    // --- unmanaged DaVinci YRGB: no output conversion, the node sees the output --
    {
        expect(isDisplay(timelineFromSettings(settings("davinciYRGB", "0", "Rec.709 (Scene)", "")), kRec709Gamma24),
               "YRGB default 'Rec.709 (Scene)' streams as Rec.709 G2.4 (BT.1886 convention)");
        expect(isDisplay(timelineFromSettings(settings("davinciYRGB", "0", "Rec.709 Gamma 2.2", "")), kRec709Gamma22),
               "YRGB 'Rec.709 Gamma 2.2'");
        expect(isDisplay(timelineFromSettings(settings("davinciYRGB", "0", "P3-D65", "")), kP3D65Gamma26),
               "Resolve's combined 'P3-D65' is P3-D65 gamma 2.6");
    }

    // --- RCM: display timelines resolve, scene/log ones are flagged ------------
    {
        const char* rcm = "davinciYRGBColorManagedv2";
        expect(isDisplay(timelineFromSettings(settings(rcm, "0", "Rec.709 Gamma 2.4", "ST2084 1000 nit")),
                         kRec709Gamma24),
               "combined mode ignores the stale gamma field Resolve still reports");
        expect(isDisplay(timelineFromSettings(settings(rcm, "0", "sRGB", "")), kSRGB), "RCM 'sRGB'");
        expect(isDisplay(timelineFromSettings(settings(rcm, "0", "Rec.2020 Gamma 2.4", "")), kRec2020Gamma24),
               "RCM 'Rec.2020 Gamma 2.4'");
        const Resolved dwg = timelineFromSettings(settings(rcm, "0", "DaVinci WG/Intermediate", ""));
        expect(dwg.kind == kSceneReferred && dwg.label == "DaVinci WG/Intermediate",
               "DaVinci WG/Intermediate is scene-referred, labelled with Resolve's name");
        expect(timelineFromSettings(settings(rcm, "0", "Rec.709 (Scene)", "")).kind == kSceneReferred,
               "under RCM, 'Rec.709 (Scene)' is scene-referred");
        expect(timelineFromSettings(settings(rcm, "0", "Linear", "")).kind == kSceneReferred, "Linear is scene");
        expect(timelineFromSettings(settings(rcm, "0", "Rec.2100 ST2084", "")).kind == kHdr, "ST2084 is HDR");
        const Resolved dci = timelineFromSettings(settings(rcm, "0", "P3-DCI", ""));
        expect(dci.kind == kUnknown && dci.label == "P3-DCI", "P3-DCI (DCI white) has no preset: unknown");
        expect(timelineFromSettings(settings(rcm, "0", "Rec.709-A", "")).kind == kUnknown,
               "Apple's Rec.709-A has no preset: unknown");
    }

    // --- RCM with separate gamut and gamma ---------------------------------------
    {
        const char* rcm = "davinciYRGBColorManagedv2";
        expect(isDisplay(timelineFromSettings(settings(rcm, "1", "P3-D65", "Gamma 2.2")), kP3D65Gamma22),
               "P3-D65 + Gamma 2.2");
        expect(isDisplay(timelineFromSettings(settings(rcm, "1", "P3-D65", "sRGB")), kDisplayP3),
               "P3-D65 + sRGB curve is Display P3");
        expect(isDisplay(timelineFromSettings(settings(rcm, "1", "Rec.709", "Gamma 2.4")), kRec709Gamma24),
               "Rec.709 + Gamma 2.4");
        expect(isDisplay(timelineFromSettings(settings(rcm, "1", "sRGB", "sRGB")), kSRGB), "sRGB + sRGB");
        expect(isDisplay(timelineFromSettings(settings(rcm, "1", "Rec.2020", "Gamma 2.4")), kRec2020Gamma24),
               "Rec.2020 + Gamma 2.4");
        expect(timelineFromSettings(settings(rcm, "1", "Rec.709", "Rec.709")).kind == kSceneReferred,
               "the Rec.709 OETF is scene-referred");
        expect(timelineFromSettings(settings(rcm, "1", "Rec.2020", "DaVinci Intermediate")).kind == kSceneReferred,
               "DaVinci Intermediate gamma is log");
        const Resolved odd = timelineFromSettings(settings(rcm, "1", "Rec.709", "Gamma 2.6"));
        expect(odd.kind == kUnknown && odd.label == "Rec.709 / Gamma 2.6", "a display combo without a preset is unknown");
        expect(timelineFromSettings(settings(rcm, "1", "Rec.2020", "Rec.2100 HLG")).kind == kHdr, "HLG is HDR");
    }

    // --- ACES: always ACEScct/ACEScc, whatever stale gamut fields say ----------
    {
        const Resolved cct = timelineFromSettings(settings("acescct", "1", "P3-DCI", "ST2084 1000 nit"));
        expect(cct.kind == kSceneReferred && cct.label == "ACEScct", "ACEScct timelines are log");
        expect(timelineFromSettings(settings("acescc", "0", "Rec.709 Gamma 2.4", "")).label == "ACEScc",
               "ACEScc timelines are log");
    }

    // --- OFX 1.5 colourspace identifiers ----------------------------------------
    {
        expect(isDisplay(fromOfxColourspace("rec1886_rec709_display"), kRec709Gamma24), "rec1886_rec709_display");
        expect(isDisplay(fromOfxColourspace("g22_rec709_tx"), kRec709Gamma22), "g22_rec709_tx");
        expect(isDisplay(fromOfxColourspace("srgb_display"), kSRGB), "srgb_display");
        expect(isDisplay(fromOfxColourspace("displayp3_display"), kDisplayP3), "displayp3_display");
        expect(isDisplay(fromOfxColourspace("p3d65_display"), kP3D65Gamma26), "p3d65_display is gamma 2.6");
        expect(isDisplay(fromOfxColourspace("rec1886_rec2020_display"), kRec2020Gamma24), "rec1886_rec2020_display");
        expect(fromOfxColourspace("davinci_intermediate_widegamut").kind == kSceneReferred,
               "davinci_intermediate_widegamut is log");
        expect(fromOfxColourspace("ACEScct").kind == kSceneReferred, "ACEScct is log");
        expect(fromOfxColourspace("lin_rec709_srgb").kind == kSceneReferred, "linear spaces are scene-referred");
        expect(fromOfxColourspace("ofx_scene_log").kind == kSceneReferred, "the basic scene-log space");
        expect(fromOfxColourspace("rec2100_pq_display").kind == kHdr, "PQ display is HDR");
        expect(fromOfxColourspace("ofx_display_sdr").kind == kUnknown, "generic SDR display names no exact space");
        expect(fromOfxColourspace("").kind == kUnknown && fromOfxColourspace("ofx_raw").kind == kUnknown,
               "empty and raw are unknown");
    }

    // --- the NDI Output choice, as projects save it --------------------------------
    {
        expect(decodeOutputChoice(0).kind == OutputChoice::kPassthrough, "choice 0 is Passthrough");
        expect(decodeOutputChoice(1).kind == OutputChoice::kPreset && decodeOutputChoice(1).space == kRec709Gamma24,
               "choice 1 is the first display space");
        expect(decodeOutputChoice(7).kind == OutputChoice::kPreset && decodeOutputChoice(7).space == kRec2020Gamma24,
               "choice 7 is Rec.2020 Gamma 2.4");
        expect(decodeOutputChoice(8).kind == OutputChoice::kLutFile, "choice 8 is LUT File, fixed forever");
        expect(decodeOutputChoice(-1).kind == OutputChoice::kPassthrough &&
                   decodeOutputChoice(99).kind == OutputChoice::kPassthrough,
               "out-of-range choices stream untouched");
        expect(decodeTimelineChoice(0) == -1 && decodeTimelineChoice(4) == kP3D65Gamma22 &&
                   decodeTimelineChoice(42) == -1,
               "Timeline Color: 0 and out-of-range mean Auto");
    }

    // --- what the stream carries, and what Stream Status says -----------------------
    {
        Resolved rec709;
        rec709.kind = kDisplay;
        rec709.space = kRec709Gamma24;
        rec709.label = "Rec.709 Gamma 2.4";
        Resolved dwg;
        dwg.kind = kSceneReferred;
        dwg.label = "DaVinci WG/Intermediate";
        const Resolved unknown;

        OutputPlan p = planOutput(decodeOutputChoice(0), rec709, nullptr);
        expect(p.inSpace == -1 && p.tagSpace == kRec709Gamma24 && p.status == "color: passthrough (Rec.709 G2.4)",
               "Passthrough of a known display timeline: no transform, tagged with the timeline space");
        p = planOutput(decodeOutputChoice(0), unknown, nullptr);
        expect(p.tagSpace == -1 && p.status.empty(), "Passthrough of an unknown timeline: no tag, no status");
        p = planOutput(decodeOutputChoice(0), dwg, nullptr);
        expect(p.tagSpace == -1 &&
                   p.status == "color: passthrough — timeline is DaVinci WG/Intermediate, not a display picture",
               "Passthrough of a log timeline says so and is not tagged");

        p = planOutput(decodeOutputChoice(4), rec709, nullptr);  // P3-D65 Gamma 2.2
        expect(p.inSpace == kRec709Gamma24 && p.outSpace == kP3D65Gamma22 && p.tagSpace == kP3D65Gamma22 &&
                   p.status == "color: Rec.709 G2.4 → P3-D65 G2.2",
               "preset from a known timeline");
        p = planOutput(decodeOutputChoice(1), rec709, nullptr);
        expect(p.inSpace == p.outSpace && p.status == "color: Rec.709 G2.4 → Rec.709 G2.4 (no conversion)",
               "preset equal to the timeline: no conversion");
        p = planOutput(decodeOutputChoice(3), unknown, nullptr);
        expect(p.inSpace == kRec709Gamma24 &&
                   p.status == "color: Rec.709 G2.4 → sRGB — timeline colour unknown, assuming Rec.709 G2.4",
               "preset from an unknown timeline assumes Rec.709 G2.4 and says so");
        p = planOutput(decodeOutputChoice(1), dwg, nullptr);
        expect(p.status == "color: Rec.709 G2.4 → Rec.709 G2.4 (no conversion) — WARNING: timeline is DaVinci "
                           "WG/Intermediate (scene-referred/log), use LUT File",
               "preset on a log timeline warns");

        LutInfo lut;
        lut.valid = true;
        lut.fileName = "show.cube";
        lut.producesSpace = kDisplayP3;
        p = planOutput(decodeOutputChoice(8), dwg, &lut);
        expect(p.tagSpace == kDisplayP3 && p.status == "color: LUT 'show.cube' → Display P3",
               "a valid LUT labels the stream with what it produces");
        LutInfo bad;
        bad.error = "line 3: expected 3 numbers, found 2";
        p = planOutput(decodeOutputChoice(8), rec709, &bad);
        expect(p.tagSpace == kRec709Gamma24 &&
                   p.status == "color: LUT invalid (line 3: expected 3 numbers, found 2) — passthrough",
               "an invalid LUT falls back to Passthrough (and its tag) with the reason");
        p = planOutput(decodeOutputChoice(8), rec709, nullptr);
        expect(p.status == "color: LUT File selected but no LUT set — passthrough" && p.tagSpace == kRec709Gamma24,
               "LUT File with no file set");
    }

    if (failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All timeline-colour tests passed\n");
    return 0;
}
