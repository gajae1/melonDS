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

#include <array>
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

#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline std::array<double, 2> DotStereoAVX2(
    const double* left, const double* right, const double* weights) noexcept
{
    __m256d l = _mm256_setzero_pd(), r = _mm256_setzero_pd();
    for (unsigned i = 0; i < 16; i += 4)
    {
        const __m256d w = _mm256_loadu_pd(weights + i);
        l = _mm256_fmadd_pd(_mm256_loadu_pd(left + i), w, l);
        r = _mm256_fmadd_pd(_mm256_loadu_pd(right + i), w, r);
    }
    const __m128d lp = _mm_add_pd(_mm256_castpd256_pd128(l), _mm256_extractf128_pd(l, 1));
    const __m128d rp = _mm_add_pd(_mm256_castpd256_pd128(r), _mm256_extractf128_pd(r, 1));
    return {_mm_cvtsd_f64(_mm_hadd_pd(lp, lp)), _mm_cvtsd_f64(_mm_hadd_pd(rp, rp))};
}
#endif

// Both channels use the same coefficients. Share loads and dispatch while
// retaining each channel's existing Dot reduction order and rounding.
inline std::array<double, 2> DotStereo(
    const double* left, const double* right, const double* weights) noexcept
{
#ifdef MELONDS_INTERPOLATION_NEON
    if (!ForcedScalar())
    {
        float64x2_t l = vdupq_n_f64(0), r = vdupq_n_f64(0);
        for (unsigned i = 0; i < 16; i += 2)
        {
            const float64x2_t w = vld1q_f64(weights + i);
            l = vfmaq_f64(l, vld1q_f64(left + i), w);
            r = vfmaq_f64(r, vld1q_f64(right + i), w);
        }
        return {vaddvq_f64(l), vaddvq_f64(r)};
    }
#elif defined(MELONDS_INTERPOLATION_AVX2)
    static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (supported && !ForcedScalar()) return DotStereoAVX2(left, right, weights);
#endif
    return {DotScalar(left, weights), DotScalar(right, weights)};
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

#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline std::array<float, 2> DotFloatStereoAVX2(
    const float* left, const float* right, const float* weights, unsigned count) noexcept
{
    __m256 l = _mm256_setzero_ps(), r = _mm256_setzero_ps();
    for (unsigned i = 0; i < count; i += 8)
    {
        const __m256 w = _mm256_loadu_ps(weights+i);
        l = _mm256_fmadd_ps(_mm256_loadu_ps(left+i), w, l);
        r = _mm256_fmadd_ps(_mm256_loadu_ps(right+i), w, r);
    }
    const __m128 lp = _mm_add_ps(_mm256_castps256_ps128(l), _mm256_extractf128_ps(l, 1));
    const __m128 rp = _mm_add_ps(_mm256_castps256_ps128(r), _mm256_extractf128_ps(r, 1));
    const __m128 lh = _mm_hadd_ps(lp, lp), rh = _mm_hadd_ps(rp, rp);
    return {_mm_cvtss_f32(_mm_hadd_ps(lh, lh)), _mm_cvtss_f32(_mm_hadd_ps(rh, rh))};
}
#endif

// The sinc output stage filters both channels with the same coefficients.
// Share weight loads and dispatch while retaining each channel's DotFloat
// accumulator sequence, reduction order and rounding.
inline std::array<float, 2> DotFloatStereo(
    const float* left, const float* right, const float* weights, unsigned count) noexcept
{
    if (ForcedScalar())
        return {DotFloatScalar(left, weights, count), DotFloatScalar(right, weights, count)};
#ifdef MELONDS_INTERPOLATION_NEON
    float32x4_t l = vdupq_n_f32(0), r = vdupq_n_f32(0);
    for (unsigned i = 0; i < count; i += 4)
    {
        const float32x4_t w = vld1q_f32(weights+i);
        l = vfmaq_f32(l, vld1q_f32(left+i), w);
        r = vfmaq_f32(r, vld1q_f32(right+i), w);
    }
    return {vaddvq_f32(l), vaddvq_f32(r)};
#else
#ifdef MELONDS_INTERPOLATION_AVX2
    static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (supported) return DotFloatStereoAVX2(left, right, weights, count);
#endif
#if defined(__SSE2__)
    __m128 l = _mm_setzero_ps(), r = _mm_setzero_ps();
    for (unsigned i = 0; i < count; i += 4)
    {
        const __m128 w = _mm_loadu_ps(weights+i);
        l = _mm_add_ps(l, _mm_mul_ps(_mm_loadu_ps(left+i), w));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps(right+i), w));
    }
    l = _mm_add_ps(l, _mm_movehl_ps(l, l));
    r = _mm_add_ps(r, _mm_movehl_ps(r, r));
    return {_mm_cvtss_f32(_mm_add_ss(l, _mm_shuffle_ps(l, l, 1))),
            _mm_cvtss_f32(_mm_add_ss(r, _mm_shuffle_ps(r, r, 1)))};
#else
    return {DotFloatScalar(left, weights, count), DotFloatScalar(right, weights, count)};
#endif
#endif
}

// Filter two phases together, sharing stereo history loads while preserving
// each dot's accumulation and reduction order. Results are A-left, A-right,
// B-left, B-right. As with DotFloatStereo, count is a multiple of 16.
#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline std::array<float, 4> DotFloatStereoPairAVX2(
    const float* left, const float* right, const float* first,
    const float* second, unsigned count) noexcept
{
    __m256 l0 = _mm256_setzero_ps(), r0 = _mm256_setzero_ps();
    __m256 l1 = _mm256_setzero_ps(), r1 = _mm256_setzero_ps();
    for (unsigned i = 0; i < count; i += 8)
    {
        const __m256 hl = _mm256_loadu_ps(left+i);
        const __m256 hr = _mm256_loadu_ps(right+i);
        const __m256 w0 = _mm256_loadu_ps(first+i);
        l0 = _mm256_fmadd_ps(hl, w0, l0);
        r0 = _mm256_fmadd_ps(hr, w0, r0);
        const __m256 w1 = _mm256_loadu_ps(second+i);
        l1 = _mm256_fmadd_ps(hl, w1, l1);
        r1 = _mm256_fmadd_ps(hr, w1, r1);
    }
    const __m128 l0p = _mm_add_ps(_mm256_castps256_ps128(l0), _mm256_extractf128_ps(l0, 1));
    const __m128 r0p = _mm_add_ps(_mm256_castps256_ps128(r0), _mm256_extractf128_ps(r0, 1));
    const __m128 l1p = _mm_add_ps(_mm256_castps256_ps128(l1), _mm256_extractf128_ps(l1, 1));
    const __m128 r1p = _mm_add_ps(_mm256_castps256_ps128(r1), _mm256_extractf128_ps(r1, 1));
    const __m128 l0h = _mm_hadd_ps(l0p, l0p), r0h = _mm_hadd_ps(r0p, r0p);
    const __m128 l1h = _mm_hadd_ps(l1p, l1p), r1h = _mm_hadd_ps(r1p, r1p);
    return {_mm_cvtss_f32(_mm_hadd_ps(l0h, l0h)), _mm_cvtss_f32(_mm_hadd_ps(r0h, r0h)),
            _mm_cvtss_f32(_mm_hadd_ps(l1h, l1h)), _mm_cvtss_f32(_mm_hadd_ps(r1h, r1h))};
}
#endif

inline std::array<float, 4> DotFloatStereoPair(
    const float* left, const float* right, const float* first,
    const float* second, unsigned count) noexcept
{
    if (ForcedScalar())
        return {DotFloatScalar(left, first, count), DotFloatScalar(right, first, count),
                DotFloatScalar(left, second, count), DotFloatScalar(right, second, count)};
#ifdef MELONDS_INTERPOLATION_NEON
    float32x4_t l0 = vdupq_n_f32(0), r0 = vdupq_n_f32(0);
    float32x4_t l1 = vdupq_n_f32(0), r1 = vdupq_n_f32(0);
    for (unsigned i = 0; i < count; i += 4)
    {
        const float32x4_t hl = vld1q_f32(left+i);
        const float32x4_t hr = vld1q_f32(right+i);
        const float32x4_t w0 = vld1q_f32(first+i);
        l0 = vfmaq_f32(l0, hl, w0);
        r0 = vfmaq_f32(r0, hr, w0);
        const float32x4_t w1 = vld1q_f32(second+i);
        l1 = vfmaq_f32(l1, hl, w1);
        r1 = vfmaq_f32(r1, hr, w1);
    }
    return {vaddvq_f32(l0), vaddvq_f32(r0), vaddvq_f32(l1), vaddvq_f32(r1)};
#else
#ifdef MELONDS_INTERPOLATION_AVX2
    static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (supported) return DotFloatStereoPairAVX2(left, right, first, second, count);
#endif
#if defined(__SSE2__)
    __m128 l0 = _mm_setzero_ps(), r0 = _mm_setzero_ps();
    __m128 l1 = _mm_setzero_ps(), r1 = _mm_setzero_ps();
    for (unsigned i = 0; i < count; i += 4)
    {
        const __m128 hl = _mm_loadu_ps(left+i);
        const __m128 hr = _mm_loadu_ps(right+i);
        const __m128 w0 = _mm_loadu_ps(first+i);
        l0 = _mm_add_ps(l0, _mm_mul_ps(hl, w0));
        r0 = _mm_add_ps(r0, _mm_mul_ps(hr, w0));
        const __m128 w1 = _mm_loadu_ps(second+i);
        l1 = _mm_add_ps(l1, _mm_mul_ps(hl, w1));
        r1 = _mm_add_ps(r1, _mm_mul_ps(hr, w1));
    }
    l0 = _mm_add_ps(l0, _mm_movehl_ps(l0, l0));
    r0 = _mm_add_ps(r0, _mm_movehl_ps(r0, r0));
    l1 = _mm_add_ps(l1, _mm_movehl_ps(l1, l1));
    r1 = _mm_add_ps(r1, _mm_movehl_ps(r1, r1));
    return {_mm_cvtss_f32(_mm_add_ss(l0, _mm_shuffle_ps(l0, l0, 1))),
            _mm_cvtss_f32(_mm_add_ss(r0, _mm_shuffle_ps(r0, r0, 1))),
            _mm_cvtss_f32(_mm_add_ss(l1, _mm_shuffle_ps(l1, l1, 1))),
            _mm_cvtss_f32(_mm_add_ss(r1, _mm_shuffle_ps(r1, r1, 1)))};
#else
    return {DotFloatScalar(left, first, count), DotFloatScalar(right, first, count),
            DotFloatScalar(left, second, count), DotFloatScalar(right, second, count)};
#endif
#endif
}
}
#endif
