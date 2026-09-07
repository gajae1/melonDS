// SPDX-License-Identifier: GPL-3.0-or-later
#include "PixelConvert.h"
#include <string.h>

#if MELONDS_PIXEL_AVX2 || MELONDS_PIXEL_AVX512
#include <immintrin.h>
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

#if MELONDS_PIXEL_AVX2
__attribute__((target("avx2")))
static void ExpandAVX2(u32* pixels, size_t count) noexcept
{
    size_t i = 0;
    for (; count - i >= 8; i += 8)
    {
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels + i));
        const __m256i r = _mm256_and_si256(_mm256_slli_epi32(c, 18), _mm256_set1_epi32(0x00FC0000));
        const __m256i g = _mm256_and_si256(_mm256_slli_epi32(c, 2), _mm256_set1_epi32(0x0000FC00));
        const __m256i b = _mm256_and_si256(_mm256_srli_epi32(c, 14), _mm256_set1_epi32(0x000000FC));
        const __m256i rgb = _mm256_or_si256(_mm256_or_si256(r, g), b);
        const __m256i low = _mm256_srli_epi32(_mm256_and_si256(rgb, _mm256_set1_epi32(0x00C0C0C0)), 6);
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

bool IsSupported(Backend backend) noexcept
{
    switch (backend)
    {
    case Backend::Auto:
    case Backend::Scalar: return true;
#if MELONDS_PIXEL_AVX2
    case Backend::AVX2: return __builtin_cpu_supports("avx2");
#endif
#if MELONDS_PIXEL_AVX512
    case Backend::AVX512: return __builtin_cpu_supports("avx512f");
#endif
    default: return false;
    }
}

Function Select(Backend backend) noexcept
{
#if MELONDS_PIXEL_AVX512
    if ((backend == Backend::Auto || backend == Backend::AVX512) && IsSupported(Backend::AVX512))
        return ExpandAVX512;
#endif
#if MELONDS_PIXEL_AVX2
    if ((backend == Backend::Auto || backend == Backend::AVX2) && IsSupported(Backend::AVX2))
        return ExpandAVX2;
#endif
    return ExpandScalar;
}
}
