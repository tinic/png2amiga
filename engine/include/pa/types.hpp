#pragma once

#include <cstddef>
#include <cstdint>

namespace pa {

// Amiga hardware convention: u16 words are the native blitter / copper unit.
// We use C++ fixed-width aliases everywhere to stay portable on host builds.
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

// Most Amiga pixel coordinates fit in int16_t (max screen ≈ 1280×512 in
// interlace). int16 saves a register on 68000/020 and matches the native
// coordinate size used by BLTxxxx / copper instructions.
struct Point {
    i16 x{};
    i16 y{};

    constexpr bool operator==(const Point&) const = default;
};

struct Rect {
    i16 x{};
    i16 y{};
    u16 w{};
    u16 h{};

    [[nodiscard]] constexpr i16 right()  const noexcept { return static_cast<i16>(x + w); }
    [[nodiscard]] constexpr i16 bottom() const noexcept { return static_cast<i16>(y + h); }
};

// Amiga chip-ram-backed byte buffer — engine-side handle. On real hardware
// the storage lives in CHIP RAM (MEMF_CHIP); on host this is a plain heap
// allocation. Never move or copy these; they're always owned by one BitMap.
struct RawBytes {
    u8* data{};
    std::size_t size{};
};

} // namespace pa
