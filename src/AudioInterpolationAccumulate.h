// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AudioInterpolationMath.h"
namespace melonDS::AudioInterpolationMath {
#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline void AccumulateAvx2(double* m, double delta, const double* w)
{
    const __m256d d = _mm256_set1_pd(delta);
    for (unsigned lane = 0; lane < 16; lane += 4)
    {
        __m256d acc = _mm256_loadu_pd(m + lane);
        acc = _mm256_fmadd_pd(_mm256_loadu_pd(w + lane), d, acc);
        _mm256_storeu_pd(m + lane, acc);
    }
}
#endif
#ifdef MELONDS_INTERPOLATION_NEON
inline void AccumulateNeon(double* m, double delta, const double* w)
{
    const float64x2_t d = vdupq_n_f64(delta);
    for (unsigned lane = 0; lane < 16; lane += 2)
        vst1q_f64(m + lane, vfmaq_f64(vld1q_f64(m + lane), vld1q_f64(w + lane), d));
}
#endif
inline void Accumulate(double* moments, double delta, const double* weights) noexcept {
#ifdef MELONDS_INTERPOLATION_NEON
    if(!ForcedScalar()){AccumulateNeon(moments,delta,weights);return;}
#elif defined(MELONDS_INTERPOLATION_AVX2)
    static const bool supported=__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if(supported&&!ForcedScalar()){AccumulateAvx2(moments,delta,weights);return;}
#endif
    for(unsigned k=0;k<16;++k)moments[k]+=delta*weights[k];
}
#ifdef MELONDS_INTERPOLATION_AVX2
__attribute__((target("avx2,fma"))) inline void AccumulateStereoAvx2(
    double* left, double* right, double dl, double dr, const double* w)
{
    const __m256d LD = _mm256_set1_pd(dl), RD = _mm256_set1_pd(dr);
    for (unsigned lane = 0; lane < 16; lane += 4)
    {
        const __m256d weights = _mm256_loadu_pd(w + lane);
        _mm256_storeu_pd(left + lane,
            _mm256_fmadd_pd(weights, LD, _mm256_loadu_pd(left + lane)));
        _mm256_storeu_pd(right + lane,
            _mm256_fmadd_pd(weights, RD, _mm256_loadu_pd(right + lane)));
    }
}
#endif
#ifdef MELONDS_INTERPOLATION_NEON
inline void AccumulateStereoNeon(
    double* left, double* right, double dl, double dr, const double* w)
{
    const float64x2_t LD = vdupq_n_f64(dl), RD = vdupq_n_f64(dr);
    for (unsigned lane = 0; lane < 16; lane += 2)
    {
        const float64x2_t weights = vld1q_f64(w + lane);
        vst1q_f64(left + lane, vfmaq_f64(vld1q_f64(left + lane), weights, LD));
        vst1q_f64(right + lane, vfmaq_f64(vld1q_f64(right + lane), weights, RD));
    }
}
#endif
// Both sides use the same weights. Share the loads and dispatch while retaining
// each side's per-element arithmetic from two sequential Accumulate calls.
inline void AccumulateStereo(double* left, double* right, double dl, double dr,
    const double* weights) noexcept {
#ifdef MELONDS_INTERPOLATION_NEON
    if(!ForcedScalar()){AccumulateStereoNeon(left,right,dl,dr,weights);return;}
#elif defined(MELONDS_INTERPOLATION_AVX2)
    static const bool supported=__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if(supported&&!ForcedScalar()){AccumulateStereoAvx2(left,right,dl,dr,weights);return;}
#endif
    for(unsigned k=0;k<16;++k){left[k]+=dl*weights[k];right[k]+=dr*weights[k];}
}
}
