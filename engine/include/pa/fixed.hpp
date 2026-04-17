#pragma once

// 24.8 signed fixed-point scalar.
//
// 1.0 is stored as 0x00000100 (= 256). 8 fractional bits → 1/256 px
// resolution. 24 integer bits → ±8,388,607 px range, plenty for any
// Amiga-screen coordinate plus accumulated sub-pixel offsets.
//
// Cheaper on m68k than 16.16: (fx * int) is one 32-bit mul with no
// shift needed. (fx * fx) uses an i64 intermediate (libgcc call on
// 68000, native mulsl on 68020+) but this path is for the small number
// of frame-rate-scale multiplies (easing, fractional velocities) rather
// than per-pixel work.
//
// Value type, trivially copyable; intentionally no hidden allocations
// or virtual overhead so it's drop-in swappable for int in hot loops.

#include "types.hpp"

namespace pa {

struct fx {
    i32 raw{};

    static constexpr unsigned kFracBits = 8;
    static constexpr i32      kOne      = 1 << kFracBits;   // 256

    static constexpr fx from_raw(i32 r)   noexcept { return {r}; }
    static constexpr fx from_int(int v)   noexcept {
        return {static_cast<i32>(v) << kFracBits};
    }
    // Compile-time constants from an exact rational, e.g. fx::from_ratio(3,4).
    static constexpr fx from_ratio(int n, int d) noexcept {
        return {(static_cast<i32>(n) << kFracBits) / d};
    }

    [[nodiscard]] constexpr int to_int()       const noexcept { return raw >> kFracBits; }
    [[nodiscard]] constexpr int to_int_round() const noexcept {
        return (raw + (1 << (kFracBits - 1))) >> kFracBits;
    }

    constexpr fx operator+(fx o)  const noexcept { return {raw + o.raw}; }
    constexpr fx operator-(fx o)  const noexcept { return {raw - o.raw}; }
    constexpr fx operator-()      const noexcept { return {-raw}; }

    // Scalar int multiply (fast path: 32-bit mulsl on 68020+, msb-truncated
    // 16×16 on 68000 if gcc can prove both fit). No shift needed because
    // the fractional part is already packed into `raw`.
    constexpr fx operator*(int k) const noexcept { return {raw * k}; }

    // fx × fx: 32×32→64 then shift right by kFracBits.
    constexpr fx operator*(fx o)  const noexcept {
        return {static_cast<i32>((static_cast<i64>(raw) * o.raw) >> kFracBits)};
    }

    constexpr fx& operator+=(fx o) noexcept { raw += o.raw; return *this; }
    constexpr fx& operator-=(fx o) noexcept { raw -= o.raw; return *this; }
    constexpr fx& operator*=(int k) noexcept { raw *= k; return *this; }

    constexpr bool operator==(fx o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(fx o) const noexcept { return raw != o.raw; }
    constexpr bool operator< (fx o) const noexcept { return raw <  o.raw; }
    constexpr bool operator<=(fx o) const noexcept { return raw <= o.raw; }
    constexpr bool operator> (fx o) const noexcept { return raw >  o.raw; }
    constexpr bool operator>=(fx o) const noexcept { return raw >= o.raw; }
};

constexpr fx operator*(int k, fx v) noexcept { return v * k; }

} // namespace pa
