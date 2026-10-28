// stereo_disparity — measure horizontal/vertical disparity between the eyes
// of a side-by-side 180x180 equirect frame (LEARNINGS.md 2026-10-28).
// Grab the live stream first:  ./build/ndi_verify --source "<host> (DaVinci" --dump frame.ppm
// Build & run:  c++ -O3 -std=c++17 docs/captures/stereo_disparity.cpp -o build/stereo_disparity
//               ./build/stereo_disparity frame.ppm [downsample=2] > patches.txt
// Correct VR180 stereo: dx = lonR - lonL is never positive (positive = eyes
// diverge), sits near 0 / slightly negative in the far field and grows more
// negative for near objects; dy ~ 0 everywhere. The stderr summary line is
// the quick read; patches.txt (lon lat dx dy ncc) supports binning by depth.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); int W, H, mx;
    if (fscanf(f, "P6 %d %d %d", &W, &H, &mx) != 3) return 1; fgetc(f);
    std::vector<unsigned char> rgb((size_t)W * H * 3); fread(rgb.data(), 1, rgb.size(), f); fclose(f);
    int ds = argc > 2 ? atoi(argv[2]) : 2;          // downsample factor
    int EW = W / 2 / ds, EH = H / ds;
    std::vector<float> L((size_t)EW * EH), R((size_t)EW * EH);
    for (int y = 0; y < EH; ++y) for (int x = 0; x < EW; ++x) {
        double sl = 0, sr = 0;
        for (int j = 0; j < ds; ++j) for (int i = 0; i < ds; ++i) {
            const unsigned char* p = &rgb[(((size_t)(y*ds+j)) * W + x*ds+i) * 3];
            const unsigned char* q = &rgb[(((size_t)(y*ds+j)) * W + W/2 + x*ds+i) * 3];
            sl += 0.2126*p[0] + 0.7152*p[1] + 0.0722*p[2];
            sr += 0.2126*q[0] + 0.7152*q[1] + 0.0722*q[2];
        }
        L[(size_t)y*EW+x] = sl/(ds*ds); R[(size_t)y*EW+x] = sr/(ds*ds);
    }
    const int P = 12, step = 16, sx = EW/6, sy = EW/30;  // half patch, grid step, search ranges
    const double degpx = 180.0 / EW;
    std::vector<double> dxs, dys;
    printf("# lon lat dx dy ncc\n");
    for (int cy = P + sy; cy < EH - P - sy; cy += step) for (int cx = P + sx; cx < EW - P - sx; cx += step) {
        double m = 0, v = 0; int n = 0;
        for (int j = -P; j <= P; ++j) for (int i = -P; i <= P; ++i) { double a = L[(size_t)(cy+j)*EW+cx+i]; m += a; v += a*a; ++n; }
        m /= n; v = v/n - m*m; if (v < 25 || m < 8) continue;   // need texture, not black
        double best = -2; int bx = 0, by = 0;
        for (int dy = -sy; dy <= sy; ++dy) for (int dx = -sx; dx <= sx; ++dx) {
            double mb = 0, vb = 0, c = 0;
            for (int j = -P; j <= P; ++j) for (int i = -P; i <= P; ++i) {
                double b = R[(size_t)(cy+dy+j)*EW+cx+dx+i]; mb += b; vb += b*b;
                c += L[(size_t)(cy+j)*EW+cx+i] * b;
            }
            mb /= n; vb = vb/n - mb*mb; if (vb < 1) continue;
            double ncc = (c/n - m*mb) / std::sqrt(v*vb);
            if (ncc > best) { best = ncc; bx = dx; by = dy; }
        }
        if (best < 0.8) continue;
        double lon = ((cx+0.5)/EW - 0.5)*180, lat = (0.5 - (cy+0.5)/EH)*180;
        printf("%.1f %.1f %.2f %.2f %.3f\n", lon, lat, bx*degpx, -by*degpx, best);
        dxs.push_back(bx*degpx); dys.push_back(-by*degpx);
    }
    auto med = [](std::vector<double> v){ std::sort(v.begin(), v.end()); return v.empty()?NAN:v[v.size()/2]; };
    auto pct = [](std::vector<double> v, double p){ std::sort(v.begin(), v.end()); return v.empty()?NAN:v[(size_t)(p*(v.size()-1))]; };
    fprintf(stderr, "matches %zu  dx(lonR-lonL) median %.2f  p5 %.2f p95 %.2f | dy median %.2f p5 %.2f p95 %.2f deg\n",
            dxs.size(), med(dxs), pct(dxs,0.05), pct(dxs,0.95), med(dys), pct(dys,0.05), pct(dys,0.95));
}
