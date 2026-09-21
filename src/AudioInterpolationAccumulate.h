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
}
