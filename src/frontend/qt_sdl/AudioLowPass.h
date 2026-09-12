/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef AUDIOLOWPASS_H
#define AUDIOLOWPASS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#if MELONDS_AUDIO_NEON && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if MELONDS_AUDIO_SSE2 || MELONDS_AUDIO_FMA
#include <immintrin.h>
#endif

// From Davey Hughes' melonDS PR #2738, revision 7f97562599a74701ce9222e0f614aa32502de10a.
// Used independently of that PR's time-stretching and audio queue changes.

// fourth-order Butterworth low-pass: two cascaded RBJ biquads, stereo with
// independent state per channel. the cutoff is smoothed rather than jumped,
// since stepping the coefficients would click. wide open leaves the output alone.
class AudioLowPass
{
public:
    // Preserve prior values; NEON is explicit-only until native benchmarking.
    enum class Backend { Auto, Scalar, FMA, SSE2, NEON };

    static bool IsSupported(Backend backend)
    {
        if (backend == Backend::Auto || backend == Backend::Scalar) return true;
#if MELONDS_AUDIO_NEON && defined(__aarch64__) && defined(__ARM_NEON)
        if (backend == Backend::NEON) return true; // Baseline AArch64 FP64 SIMD.
#endif
#if MELONDS_AUDIO_SSE2
        if (backend == Backend::SSE2) return true; // Baseline on x64.
#endif
#if MELONDS_AUDIO_FMA
        // FMA is a separate CPUID feature, and requires OS-enabled AVX state.
        if (backend == Backend::FMA)
            return __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma");
#endif
        return false;
    }

    static constexpr double kSmoothingTau = 0.05;      // cutoff smoother, seconds
    static constexpr double kBypassThreshold = 0.995;  // fraction of wide-open
    // section Q's for a fourth-order Butterworth cascade
    static constexpr double kSectionQ[2] = {0.54119610014619698, 1.3065629648763766};
    static constexpr double kMinCutoff = 20.0;

    void Init(double sampleRate, Backend backend = Backend::Auto)
    {
        Kernel = backend;
        if (Kernel == Backend::Auto)
            Kernel = IsSupported(Backend::FMA) ? Backend::FMA :
                     IsSupported(Backend::SSE2) ? Backend::SSE2 : Backend::Scalar;
        if (!IsSupported(Kernel)) Kernel = Backend::Scalar;
        SampleRate = sampleRate;
        WideOpen = std::max(0.45 * sampleRate, kMinCutoff);
        for (int s = 0; s < 2; s++)
        {
            Stages[s].z1[0] = Stages[s].z1[1] = 0.0;
            Stages[s].z2[0] = Stages[s].z2[1] = 0.0;
        }
        SetCutoffNow(WideOpen);
    }

    double WideOpenCutoff() const { return WideOpen; }
    double Cutoff() const { return CurCutoff; }
    bool Bypassed() const { return CurCutoff >= (WideOpen * kBypassThreshold); }

    // advance the smoothed cutoff by one block, then filter in place.
    // the coefficients are interpolated across the block rather than replaced
    // in one go: a transposed direct-form biquad's state encodes its past under
    // the coefficients that produced it, so a step leaves the two inconsistent
    // and the filter rings, once per block.
    void Process(int16_t* samples, int numFrames, double targetHz, double blockSeconds)
    {
        if (numFrames < 1) return;

        double from[2][5], to[2][5];
        BeginBlock(targetHz, blockSeconds, from, to);
        bool bypass = Bypassed();
        ProcessBlock(samples, numFrames, from, to, bypass);
        EndBlock(to);
    }

    // advance the cutoff and the filter state over silence, writing nothing.
    // used while muted, so unmuting neither steps the coefficients nor dumps
    // whatever was still ringing in the biquads into a buffer meant to be quiet.
    void ProcessMuted(int numFrames, double targetHz, double blockSeconds)
    {
        if (numFrames < 1) return;

        double from[2][5], to[2][5];
        BeginBlock(targetHz, blockSeconds, from, to);

        ProcessBlock(nullptr, numFrames, from, to, true);
        EndBlock(to);
    }

    void Smooth(double targetHz, double blockSeconds)
    {
        targetHz = std::clamp(targetHz, kMinCutoff, WideOpen);
        double a = 1.0 - std::exp(-blockSeconds / kSmoothingTau);
        SetCutoffNow(CurCutoff + ((targetHz - CurCutoff) * a));
    }

    void SetCutoffNow(double cutoffHz)
    {
        CurCutoff = std::clamp(cutoffHz, kMinCutoff, WideOpen);
        for (int s = 0; s < 2; s++)
            Stages[s].Design(CurCutoff, SampleRate, kSectionQ[s]);
    }

    double ProcessSample(double x, int ch)
    {
        double y = x;
        for (int s = 0; s < 2; s++)
            y = Stages[s].Run(y, ch);
        return y;
    }

private:
    void ProcessBlock(int16_t* samples, int numFrames, const double from[2][5],
                      const double to[2][5], bool bypass)
    {
        const bool changing = !std::equal(from[0], from[0] + 5, to[0]) ||
                              !std::equal(from[1], from[1] + 5, to[1]);
#if MELONDS_AUDIO_FMA
        if (Kernel == Backend::FMA)
        {
            ProcessBlockFMA(samples, numFrames, from, to, bypass, changing);
            return;
        }
#endif
#if MELONDS_AUDIO_NEON && defined(__aarch64__) && defined(__ARM_NEON)
        if (Kernel == Backend::NEON)
        {
            ProcessBlockNEON(samples, numFrames, from, to, bypass, changing);
            return;
        }
#endif
        for (int i = 0; i < numFrames; i++)
        {
            if (changing) StepCoefficients(from, to, (double)(i + 1) / numFrames);
            double stereo[2];
            for (int ch = 0; ch < 2; ch++)
            {
                // Keep the state current during bypass and silence too.
                stereo[ch] = ProcessSample(samples ? samples[i * 2 + ch] : 0.0, ch);
            }
            if (!bypass)
            {
#if MELONDS_AUDIO_SSE2
                if (Kernel == Backend::SSE2)
                {
                    StoreStereo(samples + i * 2, _mm_loadu_pd(stereo));
                    continue;
                }
#endif
                samples[i * 2] = Saturate(stereo[0]);
                samples[i * 2 + 1] = Saturate(stereo[1]);
            }
        }
    }

#if MELONDS_AUDIO_SSE2 || MELONDS_AUDIO_FMA
    static void StoreStereo(int16_t* samples, __m128d y)
    {
        // Clamp before integer conversion, then round halfway away from zero.
        y = _mm_min_pd(_mm_max_pd(y, _mm_set1_pd(-32768.0)), _mm_set1_pd(32767.0));
        const __m128d half = _mm_or_pd(_mm_and_pd(y, _mm_set1_pd(-0.0)), _mm_set1_pd(0.5));
        // Adding half can round the double immediately below 0.5 up to 1.0.
        // Values strictly inside (-0.5, 0.5) must still become silent PCM.
        const __m128d magnitude = _mm_andnot_pd(_mm_set1_pd(-0.0), y);
        const __m128d quiet = _mm_cmplt_pd(magnitude, _mm_set1_pd(0.5));
        const __m128i rounded = _mm_cvttpd_epi32(_mm_andnot_pd(quiet, _mm_add_pd(y, half)));
        const int packed = _mm_cvtsi128_si32(_mm_packs_epi32(rounded, rounded));
        std::memcpy(samples, &packed, sizeof(packed));
    }
#endif

#if MELONDS_AUDIO_FMA
    __attribute__((target("avx,fma")))
    void ProcessBlockFMA(int16_t* samples, int numFrames, const double from[2][5],
                         const double to[2][5], bool bypass, bool changing)
    {
        __m128d z1[2], z2[2];
        for (int s = 0; s < 2; ++s)
        {
            z1[s] = _mm_loadu_pd(Stages[s].z1);
            z2[s] = _mm_loadu_pd(Stages[s].z2);
        }
        for (int i = 0; i < numFrames; i++)
        {
            if (changing) StepCoefficients(from, to, (double)(i + 1) / numFrames);
            __m128d y = samples ? _mm_setr_pd(samples[i * 2], samples[i * 2 + 1]) : _mm_setzero_pd();
            for (int s = 0; s < 2; ++s)
            {
                const auto& stage = Stages[s];
                const __m128d x = y;
                y = _mm_fmadd_pd(_mm_set1_pd(stage.b0), x, z1[s]);
                z1[s] = _mm_add_pd(
                    _mm_fnmadd_pd(_mm_set1_pd(stage.a1), y, _mm_mul_pd(_mm_set1_pd(stage.b1), x)),
                    z2[s]);
                z2[s] = _mm_fnmadd_pd(_mm_set1_pd(stage.a2), y,
                                     _mm_mul_pd(_mm_set1_pd(stage.b2), x));
            }
            if (!bypass)
                StoreStereo(samples + i * 2, y);
        }
        for (int s = 0; s < 2; ++s)
        {
            _mm_storeu_pd(Stages[s].z1, z1[s]);
            _mm_storeu_pd(Stages[s].z2, z2[s]);
        }
    }
#endif

#if MELONDS_AUDIO_NEON && defined(__aarch64__) && defined(__ARM_NEON)
#if defined(__GNUC__) && !defined(__clang__)
    __attribute__((optimize("fp-contract=off")))
#endif
    void ProcessBlockNEON(int16_t* samples, int numFrames, const double from[2][5],
                          const double to[2][5], bool bypass, bool changing)
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        float64x2_t z1[2], z2[2];
        for (int s = 0; s < 2; ++s)
        {
            z1[s] = vld1q_f64(Stages[s].z1);
            z2[s] = vld1q_f64(Stages[s].z2);
        }
        for (int i = 0; i < numFrames; ++i)
        {
            if (changing) StepCoefficients(from, to, (double)(i + 1) / numFrames);
            float64x2_t y = samples
                ? float64x2_t{double(samples[i * 2]), double(samples[i * 2 + 1])}
                : vdupq_n_f64(0.0);
            // Only L/R run in parallel; samples and cascade stages remain ordered.
            for (int s = 0; s < 2; ++s)
            {
                const auto& stage = Stages[s];
                const float64x2_t x = y;
                y = vaddq_f64(vmulq_n_f64(x, stage.b0), z1[s]);
                z1[s] = vaddq_f64(vsubq_f64(vmulq_n_f64(x, stage.b1),
                                           vmulq_n_f64(y, stage.a1)), z2[s]);
                z2[s] = vsubq_f64(vmulq_n_f64(x, stage.b2), vmulq_n_f64(y, stage.a2));
            }
            if (!bypass)
            {
                y = vminq_f64(vmaxq_f64(y, vdupq_n_f64(-32768.0)), vdupq_n_f64(32767.0));
                // FCVTAS rounds halfway away from zero without adding +/-0.5.
                const int64x2_t rounded = vcvtaq_s64_f64(y);
                samples[i * 2] = static_cast<int16_t>(vgetq_lane_s64(rounded, 0));
                samples[i * 2 + 1] = static_cast<int16_t>(vgetq_lane_s64(rounded, 1));
            }
        }
        for (int s = 0; s < 2; ++s)
        {
            vst1q_f64(Stages[s].z1, z1[s]);
            vst1q_f64(Stages[s].z2, z2[s]);
        }
    }
#endif

    static int16_t Saturate(double y)
    {
        long v = std::lround(y);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        return (int16_t)v;
    }

    // coefficients as they stand, then as designed for this block's cutoff
    void BeginBlock(double targetHz, double blockSeconds, double from[2][5], double to[2][5])
    {
        for (int s = 0; s < 2; s++) Stages[s].Snapshot(from[s]);
        Smooth(targetHz, blockSeconds);
        for (int s = 0; s < 2; s++) Stages[s].Snapshot(to[s]);
    }

    void StepCoefficients(const double from[2][5], const double to[2][5], double t)
    {
        for (int s = 0; s < 2; s++) Stages[s].Lerp(from[s], to[s], t);
    }

    // land exactly on the designed set, so rounding in the interpolation
    // cannot accumulate across blocks
    void EndBlock(const double to[2][5])
    {
        for (int s = 0; s < 2; s++) Stages[s].Restore(to[s]);
    }

    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1[2] = {0.0, 0.0};
        double z2[2] = {0.0, 0.0};

        void Snapshot(double out[5]) const
        {
            out[0] = b0; out[1] = b1; out[2] = b2; out[3] = a1; out[4] = a2;
        }

        void Restore(const double in[5])
        {
            b0 = in[0]; b1 = in[1]; b2 = in[2]; a1 = in[3]; a2 = in[4];
        }

        void Lerp(const double from[5], const double to[5], double t)
        {
            b0 = from[0] + ((to[0] - from[0]) * t);
            b1 = from[1] + ((to[1] - from[1]) * t);
            b2 = from[2] + ((to[2] - from[2]) * t);
            a1 = from[3] + ((to[3] - from[3]) * t);
            a2 = from[4] + ((to[4] - from[4]) * t);
        }

        void Design(double cutoffHz, double sampleRate, double q)
        {
            double w0 = 2.0 * std::numbers::pi * (cutoffHz / sampleRate);
            double cw = std::cos(w0);
            double alpha = std::sin(w0) / (2.0 * q);
            double a0 = 1.0 + alpha;

            b0 = ((1.0 - cw) * 0.5) / a0;
            b1 = (1.0 - cw) / a0;
            b2 = b0;
            a1 = (-2.0 * cw) / a0;
            a2 = (1.0 - alpha) / a0;
        }

        // transposed direct form II
        double Run(double x, int ch)
        {
            double y = (b0 * x) + z1[ch];
            z1[ch] = (b1 * x) - (a1 * y) + z2[ch];
            z2[ch] = (b2 * x) - (a2 * y);
            return y;
        }
    };

    double SampleRate = 48000.0;
    double WideOpen = 21600.0;
    double CurCutoff = 21600.0;
    Biquad Stages[2];
    Backend Kernel = Backend::Scalar;
};

#endif // AUDIOLOWPASS_H
