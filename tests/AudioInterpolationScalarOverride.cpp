// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioInterpolationAccumulate.h"
#include <cstdio>
#include <cstring>

// BV-12 follow-up: MELONDS_INTERPOLATION_SCALAR lets a SIMD-capable host run
// the scalar kernels for field diagnosis and A/B measurement. The expected
// state arrives as argv and the variable itself through the ctest ENVIRONMENT
// property, so the same binary covers forced and automatic dispatch.
int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "forced") && std::strcmp(argv[1], "auto")))
    {
        std::fputs("usage: AudioInterpolationScalarOverride <forced|auto>\n", stderr);
        return 1;
    }
    const bool expectForced = std::strcmp(argv[1], "forced") == 0;
    if (melonDS::AudioInterpolationMath::ForcedScalar() != expectForced)
    {
        std::fputs("Scalar override state mismatch\n", stderr);
        return 2;
    }
    // Same exact-arithmetic style as AudioInterpolationSimdGate: integers and
    // powers of two stay exact under both fused and unfused evaluation, so the
    // selected kernel must still produce identical results.
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
        if (melonDS::AudioInterpolationMath::Dot(a, b) != expected) return 3;
        melonDS::AudioInterpolationMath::Accumulate(m, 0.5, a);
        for (unsigned i = 0; i < 16; ++i)
            if (m[i] != double(i) + 0.5 * a[i]) return 4;
    }
    std::puts("Interpolation scalar override PASS");
    return 0;
}
