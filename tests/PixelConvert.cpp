// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <stdio.h>
#include <vector>
#include "PixelConvert.h"
using namespace melonDS;
using namespace PixelConvert;
static u32 Reference(u32 c)
{
    const u32 r = c & 63, g = (c >> 8) & 63, b = (c >> 16) & 63;
    return 0xFF000000 | (((r * 4) | (r / 16)) << 16) |
           (((g * 4) | (g / 16)) << 8) | (b * 4) | (b / 16);
}
int main()
{
    static_assert(int(Backend::Auto) == 0 && int(Backend::Scalar) == 1 &&
                  int(Backend::AVX2) == 2 && int(Backend::AVX512) == 3 &&
                  int(Backend::AVX512F) == 4);
    for (Backend invalid : {static_cast<Backend>(-1), static_cast<Backend>(255)})
        if (IsSupported(invalid) || Select(invalid) != ExpandScalar) return 1;
    if (Select(Backend::Scalar) != ExpandScalar) return 1;
    if (IsSupported(Backend::NEON) && Select() != Select(Backend::NEON)) return 1;
    unsigned tested = 0;
    for (Backend backend : {Backend::Scalar, Backend::AVX2, Backend::AVX512, Backend::AVX512F, Backend::NEON, Backend::Auto})
    {
        printf("backend=%d native_supported=%d\n", int(backend), IsSupported(backend));
        const auto fn = Select(backend);
        if (!IsSupported(backend) && fn != ExpandScalar) return 1;
        if (backend != Backend::Scalar && backend != Backend::Auto &&
            IsSupported(backend) && fn == ExpandScalar) return 1;
        fn(nullptr, 0);
        std::vector<u32> values(64 * 64 * 64);
        for (unsigned i = 0; i < values.size(); ++i)
            values[i] = (i & 63) | (((i >> 6) & 63) << 8) | ((i >> 12) << 16) | 0xABC0C0C0;
        const auto before = values;
        fn(values.data(), values.size());
        for (unsigned i = 0; i < values.size(); ++i)
            if (values[i] != Reference(before[i])) return 2;
        for (size_t offset = 0; offset < 16; ++offset)
        for (size_t size : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255, 256, 257, 513})
        {
            // No readable SIMD-width padding at the end: sanitizers catch tail overreads.
            std::vector<u32> input(size + offset);
            u32 seed = 0xBADFEED;
            for (auto& x : input) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; x = seed; }
            const auto original = input;
            if (!input.empty()) fn(input.data() + offset, size);
            for (size_t i = 0; i < input.size(); ++i)
                if (input[i] != (i < offset ? original[i] : Reference(original[i]))) return 3;
        }
        ++tested;
    }
    printf("Pixel conversion: %u backends/selection modes; exhaustive RGB666 and tail tests PASS\n", tested);
}
