// SPDX-License-Identifier: GPL-3.0-or-later
#include "PixelConvert.h"
#include <string.h>

#if MELONDS_PIXEL_AVX2 || MELONDS_PIXEL_AVX512
#include <immintrin.h>
#endif

#if MELONDS_PIXEL_NEON && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
#include <arm_neon.h>
#endif

namespace melonDS::PixelConvert
{
void ExpandScalar(u32* pixels, size_t count) noexcept
{
    size_t i = 0;
    // Preserve the original two-pixel SWAR path without aliasing u32 storage as u64.
    for (; count - i >= 2; i += 2)
    {
        u64 c;
        memcpy(&c, pixels + i, sizeof(c));
        const u64 rgb = ((c << 18) & 0x00FC000000FC0000ULL) |
                        ((c << 2) & 0x0000FC000000FC00ULL) |
                        ((c >> 14) & 0x000000FC000000FCULL);
        c = rgb | ((rgb & 0x00C0C0C000C0C0C0ULL) >> 6) | 0xFF000000FF000000ULL;
        memcpy(pixels + i, &c, sizeof(c));
    }
    for (; i < count; ++i)
    {
        const u32 c = pixels[i];
        const u32 rgb = ((c << 18) & 0x00FC0000) |
                        ((c << 2) & 0x0000FC00) |
                        ((c >> 14) & 0x000000FC);
        pixels[i] = rgb | ((rgb & 0x00C0C0C0) >> 6) | 0xFF000000;
    }
}

#if MELONDS_PIXEL_NEON && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
static void ExpandNEON(u32* pixels, size_t count) noexcept
{
    const size_t simdCount = count & ~size_t(3);
    for (size_t i = 0; i < simdCount; i += 4)
    {
        const uint32x4_t c = vld1q_u32(pixels + i);
        // Reverse each pixel's bytes, then discard the original alpha byte.
        const uint32x4_t bgr = vshrq_n_u32(
            vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(c))), 8);
        const uint8x16_t channels = vandq_u8(vreinterpretq_u8_u32(bgr), vdupq_n_u8(63));
        const uint8x16_t rgb = vshlq_n_u8(channels, 2);
        // Replicate the top two bits of each six-bit channel into its low bits.
        const uint8x16_t expanded = vsriq_n_u8(rgb, rgb, 6);
        const uint32x4_t result = vorrq_u32(vreinterpretq_u32_u8(expanded), vdupq_n_u32(0xFF000000));
        vst1q_u32(pixels + i, result);
    }
    if (count & 3) ExpandScalar(pixels + simdCount, count & 3);
}
#endif

#if MELONDS_PIXEL_AVX2
__attribute__((target("avx2")))
static void ExpandAVX2(u32* pixels, size_t count) noexcept
{
    size_t i = 0;
    for (; count - i >= 8; i += 8)
    {
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels + i));
        const __m256i order = _mm256_broadcastsi128_si256(_mm_setr_epi8(
            2, 1, 0, -128, 6, 5, 4, -128, 10, 9, 8, -128, 14, 13, 12, -128));
        const __m256i channels = _mm256_and_si256(_mm256_shuffle_epi8(c, order),
                                                 _mm256_set1_epi32(0x003F3F3F));
        const __m256i rgb = _mm256_slli_epi32(channels, 2);
        const __m256i low = _mm256_and_si256(_mm256_srli_epi32(channels, 4),
                                            _mm256_set1_epi32(0x00030303));
        const __m256i result = _mm256_or_si256(_mm256_or_si256(rgb, low), _mm256_set1_epi32(-16777216));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(pixels + i), result);
    }
    if (i < count) ExpandScalar(pixels + i, count - i);
}
#endif

#if MELONDS_PIXEL_AVX512
__attribute__((target("avx512f")))
static void ExpandAVX512(u32* pixels, size_t count) noexcept
{
    size_t i = 0;
    for (; count - i >= 16; i += 16)
    {
        const __m512i c = _mm512_loadu_si512(pixels + i);
        const __m512i r = _mm512_and_si512(_mm512_slli_epi32(c, 18), _mm512_set1_epi32(0x00FC0000));
        const __m512i g = _mm512_and_si512(_mm512_slli_epi32(c, 2), _mm512_set1_epi32(0x0000FC00));
        const __m512i b = _mm512_and_si512(_mm512_srli_epi32(c, 14), _mm512_set1_epi32(0x000000FC));
        const __m512i rgb = _mm512_or_si512(_mm512_or_si512(r, g), b);
        const __m512i low = _mm512_srli_epi32(_mm512_and_si512(rgb, _mm512_set1_epi32(0x00C0C0C0)), 6);
        const __m512i result = _mm512_or_si512(_mm512_or_si512(rgb, low), _mm512_set1_epi32(-16777216));
        _mm512_storeu_si512(pixels + i, result);
    }
    if (i < count) ExpandScalar(pixels + i, count - i);
}
#endif

#if MELONDS_PIXEL_AVX512BW
__attribute__((target("avx512f,avx512bw")))
static void ExpandAVX512BW(u32* pixels, size_t count) noexcept
{
    size_t i = 0;
    for (; count - i >= 16; i += 16)
    {
        const __m512i c = _mm512_loadu_si512(pixels + i);
        const __m512i order = _mm512_broadcast_i32x4(_mm_setr_epi8(
            2, 1, 0, -128, 6, 5, 4, -128, 10, 9, 8, -128, 14, 13, 12, -128));
        const __m512i channels = _mm512_and_si512(_mm512_shuffle_epi8(c, order),
                                                 _mm512_set1_epi32(0x003F3F3F));
        const __m512i rgb = _mm512_slli_epi32(channels, 2);
        const __m512i low = _mm512_and_si512(_mm512_srli_epi32(channels, 4),
                                            _mm512_set1_epi32(0x00030303));
        const __m512i result = _mm512_or_si512(_mm512_or_si512(rgb, low),
                                              _mm512_set1_epi32(-16777216));
        _mm512_storeu_si512(pixels + i, result);
    }
    if (i < count) ExpandScalar(pixels + i, count - i);
}
#endif

bool IsSupported(Backend backend) noexcept
{
    switch (backend)
    {
    case Backend::Auto:
    case Backend::Scalar: return true;
#if MELONDS_PIXEL_NEON && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
    case Backend::NEON: return true;
#endif
#if MELONDS_PIXEL_AVX2
    case Backend::AVX2: return __builtin_cpu_supports("avx2");
#endif
#if MELONDS_PIXEL_AVX512
    case Backend::AVX512:
    case Backend::AVX512F: return __builtin_cpu_supports("avx512f");
#endif
    default: return false;
    }
}

Function Select([[maybe_unused]] Backend backend) noexcept
{
#if MELONDS_PIXEL_NEON && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
    if (backend == Backend::Auto || backend == Backend::NEON)
        return ExpandNEON;
#endif
#if MELONDS_PIXEL_AVX512BW
    if ((backend == Backend::Auto || backend == Backend::AVX512) &&
        __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))
        return ExpandAVX512BW;
#endif
#if MELONDS_PIXEL_AVX512
    if ((backend == Backend::Auto || backend == Backend::AVX512 || backend == Backend::AVX512F) &&
        IsSupported(Backend::AVX512))
        return ExpandAVX512;
#endif
#if MELONDS_PIXEL_AVX2
    if ((backend == Backend::Auto || backend == Backend::AVX2) && IsSupported(Backend::AVX2))
        return ExpandAVX2;
#endif
    return ExpandScalar;
}
}
