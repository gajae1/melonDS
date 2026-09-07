#pragma once

#include <bit>
#include <type_traits>

namespace std20 {

// Keep the existing call sites; delegate to the standard, zero-safe operation.
template <class T>
constexpr T log2p1(T x) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if constexpr (std::is_same_v<T, bool>)
        return x;
    else
        return static_cast<T>(std::bit_width(x));
}

} // namespace std20
