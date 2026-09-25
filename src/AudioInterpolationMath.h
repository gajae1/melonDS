// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_AUDIOINTERPOLATIONMATH_H
#define MELONDS_AUDIOINTERPOLATIONMATH_H

// Core exports consistent gates to every consumer of these inline kernels.
// Standalone coefficient tools retain automatic dispatch when not configured.
#if (!defined(MELONDS_INTERPOLATION_FMA) || MELONDS_INTERPOLATION_FMA) && \
    (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define MELONDS_INTERPOLATION_AVX2 1
#endif
#if (!defined(MELONDS_INTERPOLATION_USE_NEON) || MELONDS_INTERPOLATION_USE_NEON) && \
    defined(__aarch64__) && !defined(__ARM_BIG_ENDIAN)
#include <arm_neon.h>
#define MELONDS_INTERPOLATION_NEON 1
#endif

#include <cstdlib>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace melonDS::AudioInterpolationMath
{
// MELONDS_INTERPOLATION_SCALAR=1 forces the scalar kernels at runtime so a
// SIMD-capable host can be diagnosed or measured against the scalar path
// without rebuilding (BV-12 dispatch gap). Unset, empty, or "0" keeps the
// automatic dispatch. Read once and cached like the CPU feature checks.
inline bool ForcedScalar() noexcept
{
    static const bool forced = []
    {
        const char* value = std::getenv("MELONDS_INTERPOLATION_SCALAR");
        return value != nullptr && value[0] != '\0' &&
               !(value[0] == '0' && value[1] == '\0');
    }();
    return forced;
}

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
    if (!ForcedScalar())
    {
        float64x2_t sum = vdupq_n_f64(0);
        for (unsigned i = 0; i < 16; i += 2)
            sum = vfmaq_f64(sum, vld1q_f64(a + i), vld1q_f64(b + i));
        return vaddvq_f64(sum);
    }
#elif defined(MELONDS_INTERPOLATION_AVX2)
    static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (supported && !ForcedScalar()) return DotAVX2(a, b);
#endif
    return DotScalar(a, b);
}

// Sinc uses floats and a variable support; keep the existing double kernels
// untouched so MinimumPhase retains its exact arithmetic and PCM output.
inline float DotFloatScalar(const float* a, const float* b, unsigned count) noexcept
{
    float sum = 0;
    for (unsigned i = 0; i < count; ++i) sum += a[i] * b[i];
    return sum;
}

#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline float DotFloatAVX2(
    const float* a, const float* b, unsigned count) noexcept
{
    __m256 sum = _mm256_setzero_ps();
    for (unsigned i = 0; i < count; i += 8)
        sum = _mm256_fmadd_ps(_mm256_loadu_ps(a+i), _mm256_loadu_ps(b+i), sum);
    __m128 pair = _mm_add_ps(_mm256_castps256_ps128(sum), _mm256_extractf128_ps(sum, 1));
    pair = _mm_hadd_ps(pair, pair);
    return _mm_cvtss_f32(_mm_hadd_ps(pair, pair));
}
#endif

// count is a multiple of 16 in both the fixed and cutoff-scaled sinc paths.
inline float DotFloat(const float* a, const float* b, unsigned count) noexcept
{
    if (ForcedScalar()) return DotFloatScalar(a, b, count);
#ifdef MELONDS_INTERPOLATION_NEON
    float32x4_t sum = vdupq_n_f32(0);
    for (unsigned i = 0; i < count; i += 4)
        sum = vfmaq_f32(sum, vld1q_f32(a+i), vld1q_f32(b+i));
    return vaddvq_f32(sum);
#else
#ifdef MELONDS_INTERPOLATION_AVX2
    static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (supported) return DotFloatAVX2(a, b, count);
#endif
#if defined(__SSE2__)
    __m128 sum = _mm_setzero_ps();
    for (unsigned i = 0; i < count; i += 4)
        sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(a+i), _mm_loadu_ps(b+i)));
    sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    return _mm_cvtss_f32(_mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 1)));
#else
    return DotFloatScalar(a, b, count);
#endif
#endif
}
}
#endif
