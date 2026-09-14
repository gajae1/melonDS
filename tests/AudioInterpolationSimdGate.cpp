// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioInterpolationAccumulate.h"
#include <cstdio>

int main()
{
#if defined(MELONDS_INTERPOLATION_FMA) && !MELONDS_INTERPOLATION_FMA && defined(MELONDS_INTERPOLATION_AVX2)
    std::fputs("Disabled AVX2/FMA kernel was compiled\n", stderr);
    return 1;
#endif
#if defined(MELONDS_INTERPOLATION_USE_NEON) && !MELONDS_INTERPOLATION_USE_NEON && defined(MELONDS_INTERPOLATION_NEON)
    std::fputs("Disabled NEON kernel was compiled\n", stderr);
    return 1;
#endif
    // Naturally aligned doubles, deliberately offset from a SIMD boundary.
    // Integers and powers of two give exact expectations for both fused and
    // unfused arithmetic; numerical quality is covered by the core tests.
    alignas(32) double aStorage[17], bStorage[17], mStorage[17];
    double* a = aStorage + 1;
    double* b = bStorage + 1;
    double* m = mStorage + 1;
    for (unsigned pattern = 0; pattern < 4; ++pattern)
    {
        double expected = 0;
        for (unsigned i = 0; i < 16; ++i)
        {
            a[i] = double(i + 1) * (pattern & 1 ? -1 : 1);
            b[i] = (pattern & 2) && (i & 1) ? -0.5 : 0.25;
            m[i] = double(i);
            expected += a[i] * b[i];
        }
        if (melonDS::AudioInterpolationMath::Dot(a, b) != expected) return 2;
        melonDS::AudioInterpolationMath::Accumulate(m, 0.5, a);
        for (unsigned i = 0; i < 16; ++i)
            if (m[i] != double(i) + 0.5 * a[i]) return 3;
    }
    std::puts("Interpolation build gate and dispatched arithmetic PASS");
    return 0;
}
