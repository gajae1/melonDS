// SPDX-License-Identifier: GPL-3.0-or-later
// FS-11: screen layout contract — every layout/rotation/sizing/gap/scale/swap
// combination must produce finite in-bounds transforms, the touch inverse must
// round-trip bottom-screen points, and degenerate sizes must stay finite.
#include "ScreenLayout.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{
void Apply(const float* m, float x, float y, float& ox, float& oy)
{
    ox = x * m[0] + y * m[2] + m[4];
    oy = x * m[1] + y * m[3] + m[5];
}

bool AllFinite(const float* m)
{
    for (int i = 0; i < 6; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

// Corners of the DS source rect mapped through a screen transform.
bool CornersInBounds(const float* m, int w, int h)
{
    const float eps = 0.01f;
    for (float cx : {0.f, 256.f})
        for (float cy : {0.f, 192.f})
        {
            float ox, oy;
            Apply(m, cx, cy, ox, oy);
            if (!std::isfinite(ox) || !std::isfinite(oy)) return false;
            if (ox < -eps || ox > w + eps || oy < -eps || oy > h + eps) return false;
        }
    return true;
}

bool BottomEnabled(int sizing)
{
    return sizing != screenSizing_TopOnly;
}

int RunMatrix(int w, int h, bool checkTouch)
{
    int fails = 0;
    for (int rot = 0; rot < screenRot_MAX; ++rot)
    for (int lay = 0; lay < 4; ++lay)
    for (int sizing : {screenSizing_Even, screenSizing_EmphTop, screenSizing_EmphBot,
                       screenSizing_TopOnly, screenSizing_BotOnly})
    for (int gap : {0, 64})
    for (int iscale = 0; iscale < 2; ++iscale)
    for (int swap = 0; swap < 2; ++swap)
    {
        ScreenLayout layout;
        layout.Setup(w, h, static_cast<ScreenLayoutType>(lay),
                     static_cast<ScreenRotation>(rot),
                     static_cast<ScreenSizing>(sizing),
                     gap, iscale != 0, swap != 0, 1.f, 1.f);

        float mtx[6 * kMaxScreenTransforms];
        int kind[kMaxScreenTransforms];
        int num = layout.GetScreenTransforms(mtx, kind);
        if (num < 1 || num > kMaxScreenTransforms)
        {
            std::fprintf(stderr, "no-transform w=%d h=%d rot=%d lay=%d sizing=%d gap=%d iscale=%d swap=%d\n",
                         w, h, rot, lay, sizing, gap, iscale, swap);
            ++fails; continue;
        }
        for (int s = 0; s < num; ++s)
        {
            if (!AllFinite(mtx + 6 * s))
            {
                std::fprintf(stderr, "nonfinite w=%d h=%d rot=%d lay=%d sizing=%d gap=%d iscale=%d swap=%d screen=%d\n",
                             w, h, rot, lay, sizing, gap, iscale, swap, kind[s]);
                ++fails;
                continue;
            }
            if (!CornersInBounds(mtx + 6 * s, w, h))
            {
                const float* m = mtx + 6 * s;
                float x0, y0, x1, y1;
                Apply(m, 0.f, 0.f, x0, y0);
                Apply(m, 256.f, 192.f, x1, y1);
                std::fprintf(stderr, "oob w=%d h=%d rot=%d lay=%d sizing=%d gap=%d iscale=%d swap=%d screen=%d (0,0)->(%.2f,%.2f) (256,192)->(%.2f,%.2f)\n",
                             w, h, rot, lay, sizing, gap, iscale, swap, kind[s], x0, y0, x1, y1);
                ++fails;
            }
        }

        if (!checkTouch || !BottomEnabled(sizing)) continue;

        // A DS bottom-screen point mapped to display coords must invert back.
        // Display input is integer-quantized, so the round-trip error is
        // bounded by ~1/scale + truncation; skip sub-0.25x layouts where the
        // screen is too small to touch meaningfully.
        const float* bot = nullptr;
        for (int s = 0; s < num; ++s)
            if (kind[s] == 1) { bot = mtx + 6 * s; break; }
        if (!bot) continue;
        const float scale = std::hypot(bot[0], bot[1]);
        if (scale < 0.25f) continue;
        float px, py;
        Apply(bot, 128.f, 96.f, px, py);
        int tx = (int)px, ty = (int)py;
        if (!layout.GetTouchCoords(tx, ty, false)) { ++fails; continue; }
        const int tol = (int)std::ceil(1.f / scale) + 2;
        if (std::abs(tx - 128) > tol || std::abs(ty - 96) > tol) ++fails;
    }
    return fails;
}
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "bounds";

    if (std::strcmp(mode, "bounds") == 0)
    {
        int fails = 0;
        for (auto [w, h] : {std::pair{800, 600}, std::pair{640, 480},
                            std::pair{1000, 333}, std::pair{256, 192}})
            fails += RunMatrix(w, h, false);
        return fails ? 1 : 0;
    }
    if (std::strcmp(mode, "touch-roundtrip") == 0)
    {
        return RunMatrix(800, 600, true) ? 1 : 0;
    }
    if (std::strcmp(mode, "degenerate") == 0)
    {
        // Zero/tiny sizes must not produce NaN or inf transforms.
        int fails = 0;
        for (auto [w, h] : {std::pair{0, 0}, std::pair{1, 1}, std::pair{5, 3}})
            for (int lay = 0; lay < 4; ++lay)
            {
                ScreenLayout layout;
                layout.Setup(w, h, static_cast<ScreenLayoutType>(lay),
                             screenRot_0Deg, screenSizing_Even, 0,
                             false, false, 1.f, 1.f);
                float mtx[6 * kMaxScreenTransforms];
                int kind[kMaxScreenTransforms];
                int num = layout.GetScreenTransforms(mtx, kind);
                for (int s = 0; s < num; ++s)
                    if (!AllFinite(mtx + 6 * s)) ++fails;
            }
        return fails ? 1 : 0;
    }
    if (std::strcmp(mode, "toponly-notouch") == 0)
    {
        ScreenLayout layout;
        layout.Setup(800, 600, screenLayout_Natural, screenRot_0Deg,
                     screenSizing_TopOnly, 0, false, false, 1.f, 1.f);
        int x = 400, y = 300;
        // No bottom screen is shown; touch must report no hit.
        return layout.GetTouchCoords(x, y, false) ? 1 : 0;
    }
    return 64;
}
