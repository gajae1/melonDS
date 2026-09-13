// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_AUDIOINTERPOLATIONMATH_H
#define MELONDS_AUDIOINTERPOLATIONMATH_H

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define MELONDS_INTERPOLATION_AVX2 1
#endif
#if defined(__aarch64__) && !defined(__ARM_BIG_ENDIAN)
#include <arm_neon.h>
#define MELONDS_INTERPOLATION_NEON 1
#endif

namespace melonDS::AudioInterpolationMath
{
inline double DotScalar(const double* a, const double* b) noexcept
{
    double result = 0;
    for (unsigned i = 0; i < 16; ++i) result += a[i] * b[i];
    return result;
}

#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline double DotAVX2(const double* a, const double* b) noexcept
{
    __m256d sum = _mm256_setzero_pd();
    for (unsigned i = 0; i < 16; i += 4)
        sum = _mm256_fmadd_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i), sum);
    __m128d pair = _mm_add_pd(_mm256_castpd256_pd128(sum), _mm256_extractf128_pd(sum, 1));
    return _mm_cvtsd_f64(_mm_hadd_pd(pair, pair));
}
#endif

inline double Dot(const double* a, const double* b) noexcept
{
#ifdef MELONDS_INTERPOLATION_NEON
    float64x2_t sum = vdupq_n_f64(0);
    for (unsigned i = 0; i < 16; i += 2)
        sum = vfmaq_f64(sum, vld1q_f64(a + i), vld1q_f64(b + i));
    return vaddvq_f64(sum);
#elif defined(MELONDS_INTERPOLATION_AVX2)
    static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (supported) return DotAVX2(a, b);
#endif
    return DotScalar(a, b);
}
}
#endif
