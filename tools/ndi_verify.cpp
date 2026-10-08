// ndi_verify — reference NDI receiver for validating what the plugin puts on
// the wire (LEARNINGS.md §2 "automation roadmap"; built for the 2026-10-08
// "Half looks like quarter" signal-flow investigation).
//
// Connects to one NDI source, receives for a measurement window and reports:
//   * frame geometry (xres×yres, FourCC, declared frame rate, aspect,
//     progressive/fielded), the first time it is seen and again on any change
//   * measured frame rate, frames received vs. dropped (SDK performance
//     counters), queue depth
//   * mean luma and a busy-pixel-pair share for the last frame, so a black
//     frame is caught and an upscaled-soft one can be compared between runs
//   * optional: a dump of the last frame as a binary PPM (`--dump out.ppm`;
//     converted to PNG with `sips -s format png` when sips exists)
//   * optional: an interface byte-counter delta over the window as a bitrate
//     estimate (`--iface en0`), since the receive API exposes no bitrate
//
// `--expect WxH`, `--expect-fps N` and `--expect-fourcc UYVY` turn it into
// an assertion: a mismatch is reported and the exit code is nonzero.
//
// `--bandwidth lowest` and `--color best|fastest|uyvy|bgra` let it mirror a
// specific receiver's settings (VR.NDI Quest native uses highest + best, so
// a UYVY sender arrives there as P216).
//
// macOS developer tool; needs the NDI SDK for Apple. Build: `make ndi-verify`.
// See BUILD.md "Receiving the stream (test receivers)".

#include <cstddef> // NULL, before the SDK header (its C++ default args use it)
#include <cstdio>

#include <Processing.NDI.Lib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string source;        // substring of the NDI source name; empty = first
    std::string bandwidth = "highest";
    std::string color = "fastest";
    double durationSec = 5.0;
    double findTimeoutSec = 10.0;
    std::string dumpPath;
    std::string iface;
    int expectW = 0, expectH = 0;
    double expectFps = 0.0;
    std::string expectFourCC;
    bool listOnly = false;
    bool quiet = false;
};

void usage()
{
    std::fprintf(stderr,
        "usage: ndi_verify [--source <substring>] [--duration <s>] [--find-timeout <s>]\n"
        "                  [--bandwidth highest|lowest] [--color fastest|best|uyvy|bgra]\n"
        "                  [--expect WxH] [--expect-fps N] [--expect-fourcc UYVY]\n"
        "                  [--dump out.ppm] [--iface en0] [--list] [--quiet]\n"
        "\n"
        "Receives one NDI source and reports its on-wire geometry and cadence.\n"
        "Exit status: 0 = frames received and every --expect matched; 1 = a mismatch;\n"
        "2 = no source / no frames; 3 = bad arguments.\n");
}

bool parseArgs(int argc, char** argv, Options* o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 >= argc) { usage(); return false; }
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--source") { if (!next(&o->source)) return false; }
        else if (a == "--duration") { if (!next(&v)) return false; o->durationSec = std::atof(v.c_str()); }
        else if (a == "--find-timeout") { if (!next(&v)) return false; o->findTimeoutSec = std::atof(v.c_str()); }
        else if (a == "--bandwidth") { if (!next(&o->bandwidth)) return false; }
        else if (a == "--color") { if (!next(&o->color)) return false; }
        else if (a == "--expect") {
            if (!next(&v)) return false;
            if (std::sscanf(v.c_str(), "%dx%d", &o->expectW, &o->expectH) != 2 || o->expectW <= 0 || o->expectH <= 0) {
                std::fprintf(stderr, "--expect wants WxH, got '%s'\n", v.c_str());
                return false;
            }
        }
        else if (a == "--expect-fps") { if (!next(&v)) return false; o->expectFps = std::atof(v.c_str()); }
        else if (a == "--expect-fourcc") { if (!next(&o->expectFourCC)) return false; }
        else if (a == "--dump") { if (!next(&o->dumpPath)) return false; }
        else if (a == "--iface") { if (!next(&o->iface)) return false; }
        else if (a == "--list") { o->listOnly = true; }
        else if (a == "--quiet") { o->quiet = true; }
        else if (a == "-h" || a == "--help") { usage(); return false; }
        else { std::fprintf(stderr, "unknown argument '%s'\n", a.c_str()); usage(); return false; }
    }
    if (o->bandwidth != "highest" && o->bandwidth != "lowest") {
        std::fprintf(stderr, "--bandwidth must be highest or lowest\n");
        return false;
    }
    if (o->color != "fastest" && o->color != "best" && o->color != "uyvy" && o->color != "bgra") {
        std::fprintf(stderr, "--color must be fastest, best, uyvy or bgra\n");
        return false;
    }
    return true;
}

std::string fourccName(NDIlib_FourCC_video_type_e f)
{
    char s[5];
    s[0] = static_cast<char>(f & 0xff);
    s[1] = static_cast<char>((f >> 8) & 0xff);
    s[2] = static_cast<char>((f >> 16) & 0xff);
    s[3] = static_cast<char>((f >> 24) & 0xff);
    s[4] = 0;
    return s;
}

// Interface input-byte counter via `netstat -ibn` (macOS). Returns false when
// unavailable. Bytes are summed across the interface's rows that carry a
// Link# address line, which is the per-interface total on macOS.
bool ifaceInBytes(const std::string& iface, unsigned long long* out)
{
#ifdef _WIN32
    (void)iface; (void)out;
    return false;
#else
    const std::string cmd = "netstat -ibn 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char line[1024];
    bool found = false;
    unsigned long long total = 0;
    while (std::fgets(line, sizeof(line), p)) {
        // Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll —
        // the Address column is blank on lo0, so index the counters from the
        // end of the row: Ibytes is the 5th-from-last token.
        std::vector<std::string> tok;
        for (char* s = std::strtok(line, " \t\r\n"); s; s = std::strtok(nullptr, " \t\r\n")) tok.push_back(s);
        if (tok.size() < 9 || tok[0] != iface || tok[2].compare(0, 6, "<Link#") != 0) continue;
        total += std::strtoull(tok[tok.size() - 5].c_str(), nullptr, 10);
        found = true;
    }
    pclose(p);
    if (found) *out = total;
    return found;
#endif
}

struct Geometry {
    int w = 0, h = 0;
    NDIlib_FourCC_video_type_e fourcc = NDIlib_FourCC_video_type_UYVY;
    int rateN = 0, rateD = 1;
    float aspect = 0.0f;
    NDIlib_frame_format_type_e format = NDIlib_frame_format_type_progressive;
    bool operator==(const Geometry& o) const {
        return w == o.w && h == o.h && fourcc == o.fourcc && rateN == o.rateN && rateD == o.rateD &&
               aspect == o.aspect && format == o.format;
    }
    bool operator!=(const Geometry& o) const { return !(*this == o); }
};

void printGeometry(const char* label, const Geometry& g)
{
    const double fps = g.rateD ? static_cast<double>(g.rateN) / g.rateD : 0.0;
    std::printf("%s: %dx%d %s, declared %.3f fps (%d/%d), aspect %.4f, %s\n",
                label, g.w, g.h, fourccName(g.fourcc).c_str(), fps, g.rateN, g.rateD, g.aspect,
                g.format == NDIlib_frame_format_type_progressive ? "progressive" : "fielded");
}

// Luma plane extraction for the formats this tool asks for (fastest → UYVY /
// UYVA; best → P216 / PA16 for SpeedHQ sources; uyvy; bgra). Returns an 8-bit
// luma image (top-down) or empty when the FourCC is not handled.
std::vector<uint8_t> lumaPlane(const NDIlib_video_frame_v2_t& f)
{
    std::vector<uint8_t> y;
    if (f.xres <= 0 || f.yres <= 0 || !f.p_data) return y;
    const size_t w = static_cast<size_t>(f.xres), h = static_cast<size_t>(f.yres);
    y.resize(w * h);
    switch (f.FourCC) {
        case NDIlib_FourCC_video_type_UYVY:
        case NDIlib_FourCC_video_type_UYVA: {
            const int stride = f.line_stride_in_bytes ? f.line_stride_in_bytes : f.xres * 2;
            for (size_t r = 0; r < h; ++r) {
                const uint8_t* row = f.p_data + r * static_cast<size_t>(stride);
                for (size_t x = 0; x < w; ++x) y[r * w + x] = row[x * 2 + 1];
            }
            return y;
        }
        case NDIlib_FourCC_video_type_P216:
        case NDIlib_FourCC_video_type_PA16: {
            const int stride = f.line_stride_in_bytes ? f.line_stride_in_bytes : f.xres * 2;
            for (size_t r = 0; r < h; ++r) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(f.p_data + r * static_cast<size_t>(stride));
                for (size_t x = 0; x < w; ++x) y[r * w + x] = static_cast<uint8_t>(row[x] >> 8);
            }
            return y;
        }
        case NDIlib_FourCC_video_type_BGRA:
        case NDIlib_FourCC_video_type_BGRX:
        case NDIlib_FourCC_video_type_RGBA:
        case NDIlib_FourCC_video_type_RGBX: {
            const bool bgr = (f.FourCC == NDIlib_FourCC_video_type_BGRA || f.FourCC == NDIlib_FourCC_video_type_BGRX);
            const int stride = f.line_stride_in_bytes ? f.line_stride_in_bytes : f.xres * 4;
            for (size_t r = 0; r < h; ++r) {
                const uint8_t* row = f.p_data + r * static_cast<size_t>(stride);
                for (size_t x = 0; x < w; ++x) {
                    const uint8_t* px = row + x * 4;
                    const float rr = bgr ? px[2] : px[0], gg = px[1], bb = bgr ? px[0] : px[2];
                    y[r * w + x] = static_cast<uint8_t>(std::min(255.0f, 0.2126f * rr + 0.7152f * gg + 0.0722f * bb));
                }
            }
            return y;
        }
        default:
            y.clear();
            return y;
    }
}

// Writes the frame as a binary PPM (RGB8, top-down). UYVY/P216 are converted
// with Rec.709 limited-range maths (the same as the plugin's encoder and the
// Quest app's decoder); RGB formats pass through.
bool writePPM(const std::string& path, const NDIlib_video_frame_v2_t& f)
{
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    const int w = f.xres, h = f.yres;
    std::fprintf(out, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
    auto clamp8 = [](float v) { return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v + 0.5f))); };
    auto yuv = [&](float Y, float U, float V, uint8_t* px) {
        const float y = (Y - 16.0f) / 219.0f, u = (U - 128.0f) / 112.0f, v = (V - 128.0f) / 112.0f;
        const float r = y + v * (1.0f - 0.2126f);
        const float b = y + u * (1.0f - 0.0722f);
        const float g = (y - 0.2126f * r - 0.0722f * b) / (1.0f - 0.2126f - 0.0722f);
        px[0] = clamp8(r * 255.0f); px[1] = clamp8(g * 255.0f); px[2] = clamp8(b * 255.0f);
    };
    for (int r = 0; r < h; ++r) {
        switch (f.FourCC) {
            case NDIlib_FourCC_video_type_UYVY:
            case NDIlib_FourCC_video_type_UYVA: {
                const int stride = f.line_stride_in_bytes ? f.line_stride_in_bytes : w * 2;
                const uint8_t* src = f.p_data + static_cast<size_t>(r) * stride;
                for (int x = 0; x < w; ++x) {
                    const uint8_t* pair = src + (x & ~1) * 2;
                    yuv(src[x * 2 + 1], pair[0], pair[2], &row[static_cast<size_t>(x) * 3]);
                }
                break;
            }
            case NDIlib_FourCC_video_type_P216:
            case NDIlib_FourCC_video_type_PA16: {
                const int stride = f.line_stride_in_bytes ? f.line_stride_in_bytes : w * 2;
                const uint16_t* ly = reinterpret_cast<const uint16_t*>(f.p_data + static_cast<size_t>(r) * stride);
                const uint16_t* cc = reinterpret_cast<const uint16_t*>(f.p_data + static_cast<size_t>(stride) * h + static_cast<size_t>(r) * stride);
                for (int x = 0; x < w; ++x) {
                    const int c = (x & ~1); // chroma pair index * 2 → (Cb, Cr) interleaved
                    yuv(ly[x] / 256.0f, cc[c] / 256.0f, cc[c + 1] / 256.0f, &row[static_cast<size_t>(x) * 3]);
                }
                break;
            }
            case NDIlib_FourCC_video_type_BGRA:
            case NDIlib_FourCC_video_type_BGRX:
            case NDIlib_FourCC_video_type_RGBA:
            case NDIlib_FourCC_video_type_RGBX: {
                const bool bgr = (f.FourCC == NDIlib_FourCC_video_type_BGRA || f.FourCC == NDIlib_FourCC_video_type_BGRX);
                const int stride = f.line_stride_in_bytes ? f.line_stride_in_bytes : w * 4;
                const uint8_t* src = f.p_data + static_cast<size_t>(r) * stride;
                for (int x = 0; x < w; ++x) {
                    const uint8_t* px = src + x * 4;
                    row[static_cast<size_t>(x) * 3 + 0] = bgr ? px[2] : px[0];
                    row[static_cast<size_t>(x) * 3 + 1] = px[1];
                    row[static_cast<size_t>(x) * 3 + 2] = bgr ? px[0] : px[2];
                }
                break;
            }
            default:
                std::fclose(out);
                return false;
        }
        std::fwrite(row.data(), 1, row.size(), out);
    }
    std::fclose(out);
    return true;
}

// Mean luma (black-frame check) and the share of horizontally adjacent luma
// pairs that differ by at least 8 codes ("busy pairs"). A nearest-neighbour
// 2× upscale makes every other pair identical, so a frame whose busy share is
// near zero while its edges are strong has been upscaled; compare the number
// between a known-good receiver and the one under test rather than reading it
// on its own. The chart (scripts/gen_resolution_chart.py) is the precise
// instrument; this is a smoke number.
void frameStats(const std::vector<uint8_t>& y, int w, int h, double* meanLuma, double* busyShare)
{
    *meanLuma = 0.0;
    *busyShare = 0.0;
    if (y.empty() || w < 2 || h < 1) return;
    double sum = 0.0;
    size_t busy = 0, pairs = 0;
    for (int r = 0; r < h; ++r) {
        const uint8_t* row = y.data() + static_cast<size_t>(r) * w;
        for (int x = 0; x < w; ++x) sum += row[x];
        for (int x = 0; x + 1 < w; ++x) {
            if (std::abs(static_cast<int>(row[x]) - row[x + 1]) >= 8) ++busy;
            ++pairs;
        }
    }
    *meanLuma = sum / (static_cast<double>(w) * h);
    if (pairs) *busyShare = static_cast<double>(busy) / static_cast<double>(pairs);
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, &opt)) return 3;

    if (!NDIlib_initialize()) {
        std::fprintf(stderr, "NDIlib_initialize failed (CPU unsupported?)\n");
        return 2;
    }

    NDIlib_find_create_t findDesc;
    findDesc.show_local_sources = true;
    findDesc.p_groups = nullptr;
    findDesc.p_extra_ips = nullptr;
    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findDesc);
    if (!finder) {
        std::fprintf(stderr, "NDIlib_find_create_v2 failed\n");
        return 2;
    }

    // Discover. Sources appear over a few seconds; keep polling until the
    // wanted one is present or the timeout passes.
    const NDIlib_source_t* sources = nullptr;
    uint32_t count = 0;
    int chosen = -1;
    const auto findDeadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(opt.findTimeoutSec * 1000));
    while (Clock::now() < findDeadline) {
        NDIlib_find_wait_for_sources(finder, 500);
        sources = NDIlib_find_get_current_sources(finder, &count);
        chosen = -1;
        for (uint32_t i = 0; i < count; ++i) {
            const std::string name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
            if (opt.source.empty() || name.find(opt.source) != std::string::npos) { chosen = static_cast<int>(i); break; }
        }
        if (chosen >= 0 && !opt.listOnly) break;
    }
    if (opt.listOnly || chosen < 0) {
        std::printf("NDI sources visible (%u):\n", count);
        for (uint32_t i = 0; i < count; ++i) {
            std::printf("  %s%s\n", sources[i].p_ndi_name ? sources[i].p_ndi_name : "?",
                        (opt.source.empty() ? false : std::string(sources[i].p_ndi_name ? sources[i].p_ndi_name : "").find(opt.source) != std::string::npos) ? "   <- match" : "");
        }
        if (opt.listOnly) { NDIlib_find_destroy(finder); NDIlib_destroy(); return 0; }
        std::fprintf(stderr, "no source%s%s%s found within %.0f s\n", opt.source.empty() ? "" : " matching '",
                     opt.source.c_str(), opt.source.empty() ? "" : "'", opt.findTimeoutSec);
        NDIlib_find_destroy(finder);
        NDIlib_destroy();
        return 2;
    }

    const std::string sourceName = sources[chosen].p_ndi_name;
    std::printf("source: %s\n", sourceName.c_str());

    NDIlib_recv_create_v3_t rc;
    rc.source_to_connect_to = sources[chosen];
    rc.bandwidth = (opt.bandwidth == "lowest") ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
    rc.color_format = NDIlib_recv_color_format_fastest;
    if (opt.color == "best") rc.color_format = NDIlib_recv_color_format_best;
    else if (opt.color == "uyvy") rc.color_format = NDIlib_recv_color_format_UYVY_BGRA;
    else if (opt.color == "bgra") rc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    rc.allow_video_fields = false;
    rc.p_ndi_recv_name = "ndi_verify";
    NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&rc);
    NDIlib_find_destroy(finder); // sources[] belongs to the finder; copied into rc above
    if (!recv) {
        std::fprintf(stderr, "NDIlib_recv_create_v3 failed\n");
        NDIlib_destroy();
        return 2;
    }
    std::printf("receiver: bandwidth=%s color=%s\n", opt.bandwidth.c_str(), opt.color.c_str());

    unsigned long long ifaceStart = 0;
    bool haveIface = !opt.iface.empty() && ifaceInBytes(opt.iface, &ifaceStart);
    if (!opt.iface.empty() && !haveIface) {
        std::fprintf(stderr, "warning: interface '%s' not found in netstat -ibn; skipping bitrate estimate\n", opt.iface.c_str());
    }

    // Receive. The window starts at the first video frame so connection
    // latency doesn't dilute the measured rate.
    Geometry geom, firstGeom;
    bool haveGeom = false;
    int geometryChanges = 0;
    uint64_t frames = 0;
    std::vector<uint8_t> lastLuma;
    NDIlib_video_frame_v2_t keep{};
    std::vector<uint8_t> keepData; // copy of the last frame for --dump
    Clock::time_point windowStart;
    Clock::time_point lastFrameAt;
    const auto connectDeadline = Clock::now() + std::chrono::seconds(15);
    bool started = false;
    std::vector<double> gapsMs;

    while (true) {
        const auto now = Clock::now();
        if (started) {
            if (now - windowStart >= std::chrono::milliseconds(static_cast<int>(opt.durationSec * 1000))) break;
        } else if (now > connectDeadline) {
            break;
        }
        NDIlib_video_frame_v2_t v{};
        const NDIlib_frame_type_e t = NDIlib_recv_capture_v3(recv, &v, nullptr, nullptr, 1000);
        if (t != NDIlib_frame_type_video) {
            if (t == NDIlib_frame_type_none && !opt.quiet) std::printf("  (no frame for 1 s)\n");
            continue;
        }
        Geometry g;
        g.w = v.xres; g.h = v.yres; g.fourcc = v.FourCC; g.rateN = v.frame_rate_N; g.rateD = v.frame_rate_D;
        g.aspect = v.picture_aspect_ratio; g.format = v.frame_format_type;
        const auto arrived = Clock::now();
        if (!started) {
            started = true;
            windowStart = arrived;
            firstGeom = g;
        } else {
            gapsMs.push_back(std::chrono::duration<double, std::milli>(arrived - lastFrameAt).count());
        }
        lastFrameAt = arrived;
        ++frames;
        if (!haveGeom || g != geom) {
            if (haveGeom) ++geometryChanges;
            geom = g;
            haveGeom = true;
            printGeometry(frames == 1 ? "first frame" : "geometry changed", geom);
        }
        {
            // Keep the last frame's pixels: the content stats always need
            // them, and --dump writes them out at the end.
            const int stride = v.line_stride_in_bytes;
            size_t bytes = 0;
            switch (v.FourCC) {
                case NDIlib_FourCC_video_type_P216:
                case NDIlib_FourCC_video_type_PA16:
                    bytes = static_cast<size_t>(stride) * v.yres * (v.FourCC == NDIlib_FourCC_video_type_PA16 ? 3 : 2);
                    break;
                case NDIlib_FourCC_video_type_UYVA:
                    bytes = static_cast<size_t>(stride) * v.yres + static_cast<size_t>(v.xres) * v.yres;
                    break;
                default:
                    bytes = static_cast<size_t>(stride) * v.yres;
                    break;
            }
            if (v.p_data && bytes) {
                keepData.assign(v.p_data, v.p_data + bytes);
                keep = v;
                keep.p_data = keepData.data();
                keep.p_metadata = nullptr;
            }
        }
        NDIlib_recv_free_video_v2(recv, &v);
    }

    NDIlib_recv_performance_t total{}, dropped{};
    NDIlib_recv_get_performance(recv, &total, &dropped);
    NDIlib_recv_queue_t queue{};
    NDIlib_recv_get_queue(recv, &queue);

    int exitCode = 0;
    if (!started) {
        std::printf("no video frames received from '%s' within 15 s\n", sourceName.c_str());
        exitCode = 2;
    } else {
        const double windowSec = std::chrono::duration<double>(lastFrameAt - windowStart).count();
        const double measuredFps = (frames > 1 && windowSec > 0.0) ? static_cast<double>(frames - 1) / windowSec : 0.0;
        double gapMed = 0.0, gapMax = 0.0;
        if (!gapsMs.empty()) {
            std::vector<double> s = gapsMs;
            std::sort(s.begin(), s.end());
            gapMed = s[s.size() / 2];
            gapMax = s.back();
        }
        double meanLuma = 0.0, busyShare = 0.0;
        if (keep.p_data) {
            lastLuma = lumaPlane(keep);
            frameStats(lastLuma, keep.xres, keep.yres, &meanLuma, &busyShare);
        }
        printGeometry("last frame", geom);
        std::printf("received: %llu frames in %.2f s = %.2f fps measured; frame gap median %.1f ms, max %.1f ms\n",
                    static_cast<unsigned long long>(frames), windowSec, measuredFps, gapMed, gapMax);
        std::printf("sdk counters: video total %lld, dropped %lld (receiver too slow to dequeue); queue depth %d\n",
                    static_cast<long long>(total.video_frames), static_cast<long long>(dropped.video_frames), queue.video_frames);
        if (geometryChanges) std::printf("geometry changed %d time(s) during the window\n", geometryChanges);
        if (keep.p_data) {
            std::printf("last frame content: mean luma %.1f/255, busy adjacent-pixel pairs %.2f%%%s\n", meanLuma,
                        busyShare * 100.0,
                        lastLuma.empty() ? " (FourCC not analysed)" : (meanLuma < 1.0 ? "  <- BLACK" : ""));
        }
        if (haveIface) {
            unsigned long long ifaceEnd = 0;
            if (ifaceInBytes(opt.iface, &ifaceEnd) && ifaceEnd >= ifaceStart && windowSec > 0.0) {
                const double mbps = static_cast<double>(ifaceEnd - ifaceStart) * 8.0 / 1e6 / windowSec;
                std::printf("interface %s: %.1f Mbit/s received over the window (all traffic on that interface)\n",
                            opt.iface.c_str(), mbps);
            }
        }

        // Assertions.
        if (opt.expectW && (geom.w != opt.expectW || geom.h != opt.expectH)) {
            std::printf("MISMATCH: expected %dx%d, got %dx%d\n", opt.expectW, opt.expectH, geom.w, geom.h);
            exitCode = 1;
        }
        if (opt.expectFps > 0.0) {
            const double declared = geom.rateD ? static_cast<double>(geom.rateN) / geom.rateD : 0.0;
            if (std::fabs(declared - opt.expectFps) > 0.01) {
                std::printf("MISMATCH: expected declared %.3f fps, got %.3f\n", opt.expectFps, declared);
                exitCode = 1;
            }
        }
        if (!opt.expectFourCC.empty() && fourccName(geom.fourcc) != opt.expectFourCC) {
            std::printf("MISMATCH: expected FourCC %s, got %s\n", opt.expectFourCC.c_str(), fourccName(geom.fourcc).c_str());
            exitCode = 1;
        }
        if (keep.p_data && !lastLuma.empty() && meanLuma < 1.0) {
            std::printf("MISMATCH: last frame is black\n");
            exitCode = 1;
        }
        if (exitCode == 0) std::printf("OK\n");

        if (!opt.dumpPath.empty() && keep.p_data) {
            if (writePPM(opt.dumpPath, keep)) {
                std::printf("dumped last frame to %s\n", opt.dumpPath.c_str());
#ifndef _WIN32
                if (opt.dumpPath.size() > 4 && opt.dumpPath.compare(opt.dumpPath.size() - 4, 4, ".ppm") == 0 &&
                    access("/usr/bin/sips", X_OK) == 0) {
                    const std::string png = opt.dumpPath.substr(0, opt.dumpPath.size() - 4) + ".png";
                    const std::string cmd = "/usr/bin/sips -s format png '" + opt.dumpPath + "' --out '" + png + "' >/dev/null 2>&1";
                    if (std::system(cmd.c_str()) == 0) std::printf("converted to %s\n", png.c_str());
                }
#endif
            } else {
                std::printf("could not write %s (unsupported FourCC or I/O error)\n", opt.dumpPath.c_str());
            }
        }
    }

    NDIlib_recv_destroy(recv);
    NDIlib_destroy();
    return exitCode;
}
