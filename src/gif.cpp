#include "gif.hpp"

#include "color_space.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace png2amiga::gif {

namespace {

// Round a palette colour count up to the next power of two so the GIF
// global-colour-table size field (3 bits) can address it. GIF requires
// the GCT to be 2^(n+1) entries (2..256) — wasted entries are zeroed.
std::uint8_t gct_size_field(std::size_t palette_count) {
    std::size_t n = 0;
    std::size_t cap = 2;
    while (cap < palette_count && n < 7) {
        cap <<= 1;
        ++n;
    }
    return static_cast<std::uint8_t>(n);
}

// LZW encoder for GIF image data. The encoder tracks a string-table
// dictionary keyed by (prefix-code, next-byte) → new-code; a packed
// bitstream output (variable-width codes, LSB-first within each byte);
// and the GIF clear/EOI control codes that reset the dictionary when
// it overflows or terminate the stream. Output is the LZW-coded byte
// vector minus the framing — caller wraps it in GIF sub-blocks.
class LzwEncoder {
public:
    LzwEncoder(std::uint8_t initial_code_size_bits)
        : initial_bits_(initial_code_size_bits == 1 ? 2
                       : initial_code_size_bits)  // GIF spec: min 2 bits
        , clear_code_(1u << initial_bits_)
        , eoi_code_(clear_code_ + 1)
        , next_code_(eoi_code_ + 1)
        , code_bits_(initial_bits_ + 1)
    {
    }

    void emit_clear() {
        write_code(clear_code_);
        dict_.clear();
        next_code_ = eoi_code_ + 1;
        code_bits_ = static_cast<std::uint8_t>(initial_bits_ + 1);
    }

    void encode(std::span<const std::uint8_t> indices) {
        emit_clear();
        if (indices.empty()) {
            write_code(eoi_code_);
            flush();
            return;
        }
        std::uint32_t prefix = indices[0];
        for (std::size_t i = 1; i < indices.size(); ++i) {
            std::uint8_t k = indices[i];
            std::uint64_t key = (static_cast<std::uint64_t>(prefix) << 8) | k;
            auto it = dict_.find(key);
            if (it != dict_.end()) {
                prefix = it->second;
                continue;
            }
            // Not in dictionary — emit prefix and add (prefix, k) → new_code.
            write_code(prefix);
            if (next_code_ < kMaxCode) {
                dict_[key] = next_code_++;
                // Width grows when the next code emitted will need
                // an extra bit. Note: GIF defers the width bump until
                // we actually emit a code that fits in the new width.
                if (next_code_ > (1u << code_bits_) && code_bits_ < 12) {
                    ++code_bits_;
                }
            } else {
                // Dictionary full — emit clear and reset.
                emit_clear();
            }
            prefix = k;
        }
        write_code(prefix);
        write_code(eoi_code_);
        flush();
    }

    std::vector<std::uint8_t> finish() {
        return std::move(out_);
    }

private:
    static constexpr std::uint32_t kMaxCode = 1u << 12;  // 4096

    void write_code(std::uint32_t code) {
        bit_buffer_ |= static_cast<std::uint64_t>(code) << bits_in_buffer_;
        bits_in_buffer_ += code_bits_;
        while (bits_in_buffer_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(bit_buffer_ & 0xFF));
            bit_buffer_ >>= 8;
            bits_in_buffer_ -= 8;
        }
    }

    void flush() {
        if (bits_in_buffer_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(bit_buffer_ & 0xFF));
            bit_buffer_ = 0;
            bits_in_buffer_ = 0;
        }
    }

    std::uint8_t  initial_bits_;
    std::uint32_t clear_code_;
    std::uint32_t eoi_code_;
    std::uint32_t next_code_;
    std::uint8_t  code_bits_;

    std::uint64_t bit_buffer_{0};
    std::uint8_t  bits_in_buffer_{0};
    std::vector<std::uint8_t> out_;
    std::unordered_map<std::uint64_t, std::uint32_t> dict_;
};

// Wrap raw LZW byte stream into GIF sub-blocks (max 255 bytes each,
// terminated by a zero-length block).
void write_lzw_subblocks(std::vector<std::uint8_t>& out,
                         std::span<const std::uint8_t> lzw) {
    std::size_t off = 0;
    while (off < lzw.size()) {
        std::size_t take = std::min<std::size_t>(255, lzw.size() - off);
        out.push_back(static_cast<std::uint8_t>(take));
        auto first = lzw.begin() +
            static_cast<std::ptrdiff_t>(off);
        auto last = first + static_cast<std::ptrdiff_t>(take);
        out.insert(out.end(), first, last);
        off += take;
    }
    out.push_back(0);  // block terminator
}

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void put_u16le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

}  // namespace

Result<std::vector<std::uint8_t>>
encode_palettized(std::span<const std::uint8_t> indices,
                  std::span<const Color3f> palette,
                  std::size_t width, std::size_t height) {
    if (width == 0 || height == 0
        || indices.size() != width * height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "gif::encode_palettized: indices size doesn't match WxH",
        }};
    }
    if (palette.empty() || palette.size() > 256) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "gif::encode_palettized: palette must be 1..256 entries",
        }};
    }
    if (width > 0xFFFF || height > 0xFFFF) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "gif::encode_palettized: dims exceed GIF 65535 limit",
        }};
    }

    std::vector<std::uint8_t> out;
    out.reserve(width * height + 1024);

    // ---- Header: "GIF89a" ----
    constexpr std::array<std::uint8_t, 6> sig{'G', 'I', 'F', '8', '9', 'a'};
    out.insert(out.end(), sig.begin(), sig.end());

    // ---- Logical Screen Descriptor ----
    // u16 width, u16 height, packed, bg index, pixel aspect (0).
    put_u16le(out, static_cast<std::uint16_t>(width));
    put_u16le(out, static_cast<std::uint16_t>(height));

    std::uint8_t gct_size = gct_size_field(palette.size());
    std::size_t  gct_count = std::size_t{1} << (gct_size + 1);  // 2..256
    // packed: GCT-flag (1) | colour-resolution (3) | sort-flag (0) |
    //         GCT-size (3). Colour resolution is "bits per primary" -1;
    //         standard value for an 8-bit-per-channel palette is 7.
    std::uint8_t packed = static_cast<std::uint8_t>(0x80
                        | (0x07 << 4)
                        | gct_size);
    put_u8(out, packed);
    put_u8(out, 0);  // background colour index
    put_u8(out, 0);  // pixel aspect ratio (0 = no info)

    // ---- Global Colour Table: gct_count × 3 bytes (sRGB 8-bit) ----
    auto srgb8 = [](float v) -> std::uint8_t {
        int q = static_cast<int>(std::lround(v * 255.0f));
        if (q < 0) q = 0;
        if (q > 255) q = 255;
        return static_cast<std::uint8_t>(q);
    };
    for (std::size_t i = 0; i < gct_count; ++i) {
        if (i < palette.size()) {
            auto srgb = color_space::linear_to_srgb(palette[i]);
            put_u8(out, srgb8(srgb.r));
            put_u8(out, srgb8(srgb.g));
            put_u8(out, srgb8(srgb.b));
        } else {
            put_u8(out, 0); put_u8(out, 0); put_u8(out, 0);
        }
    }

    // ---- Image Descriptor ----
    put_u8(out, 0x2C);                   // image-separator
    put_u16le(out, 0);                    // left
    put_u16le(out, 0);                    // top
    put_u16le(out, static_cast<std::uint16_t>(width));
    put_u16le(out, static_cast<std::uint16_t>(height));
    put_u8(out, 0);  // packed: no LCT, no interlace, no sort, no LCT-size

    // ---- LZW image data ----
    // LZW initial code size = bits needed to address palette (min 2).
    std::uint8_t lzw_min_bits = std::max<std::uint8_t>(
        2, static_cast<std::uint8_t>(gct_size + 1));
    put_u8(out, lzw_min_bits);
    LzwEncoder lzw(lzw_min_bits);
    lzw.encode(indices);
    auto lzw_bytes = lzw.finish();
    write_lzw_subblocks(out, lzw_bytes);

    // ---- Trailer ----
    put_u8(out, 0x3B);
    return out;
}

Result<void>
save_palettized(std::string_view path,
                std::span<const std::uint8_t> indices,
                std::span<const Color3f> palette,
                std::size_t width, std::size_t height) {
    auto buf = encode_palettized(indices, palette, width, height);
    if (!buf) return std::unexpected{buf.error()};
    std::ofstream f(std::string(path), std::ios::binary);
    if (!f) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            std::string("gif::save_palettized: cannot open ") + std::string(path),
        }};
    }
    f.write(reinterpret_cast<const char*>(buf->data()),
            static_cast<std::streamsize>(buf->size()));
    if (!f) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            std::string("gif::save_palettized: write failed: ") + std::string(path),
        }};
    }
    return {};
}

}  // namespace png2amiga::gif
