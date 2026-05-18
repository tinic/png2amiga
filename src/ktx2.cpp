// KTX2 container writer — see ktx2.hpp for layout.
//
// Spec: https://github.khronos.org/KTX-Specification/ — appendix tables
// for Khronos Data Format Specification (KDFS) color models / transfer
// functions / channel types.

#include "ktx2.hpp"

#include <zstd.h>

#include <cstring>

namespace png2amiga::ktx2 {

namespace {

constexpr std::uint8_t kIdentifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};

// KDFS color-model IDs (KDFS v1.3 §15.1).
// KHR_DF_MODEL_* values from khr_df.h.
constexpr std::uint32_t kModelBc1a = 128;
constexpr std::uint32_t kModelBc3 = 130;        // KHR_DF_MODEL_BC3
constexpr std::uint32_t kModelBc7 = 134;
constexpr std::uint32_t kModelEtc2 = 161;
constexpr std::uint32_t kModelAstc = 162;

// KDFS color-primary IDs.
constexpr std::uint32_t kPrimariesBT709 = 1;

// KDFS transfer-function IDs.
constexpr std::uint32_t kTransferLinear = 1;
constexpr std::uint32_t kTransferSrgb = 2;

// KDFS ETC2 channel types (KDFS v1.3 §15.4 ETC2).
// KHR_DF_CHANNEL_ETC2_COLOR (= 2) is the combined-RGB sample type used
// when a single sample covers all three colour channels of an ETC2 block.
// 0 = ETC2_RED, 1 = GREEN — those split colour into separate planes
// which our R8G8B8 vkFormat doesn't do. Verified against khr_df.h.
constexpr std::uint8_t kEtc2ChannelColor = 2;

// KHR_DF_CHANNEL_BC1A_COLOR = 0 — the single RGB sample for BC1 blocks.
// (BC1A_ALPHA = 1 is used for the 1-bit-alpha BC1A variant, not for
// VK_FORMAT_BC1_RGB_*.)
constexpr std::uint8_t kBc1ChannelColor = 0;

// KHR_DF_CHANNEL_BC3_COLOR / _ALPHA. We store as a single RGBA sample
// (channel type "color") same as BC7 — the decoder splits internally.
constexpr std::uint8_t kBc3ChannelColor = 0;

// KHR_DF_CHANNEL_BC7_COLOR = 0 — the single RGBA sample for BC7 blocks.
constexpr std::uint8_t kBc7ChannelColor = 0;

// Helpers — pack little-endian integers into the output buffer.
void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu));
    }
}
void write_u32_at(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    out[at + 0] = static_cast<std::uint8_t>(v & 0xFF);
    out[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    out[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}
void write_u64_at(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
        out[at + i] = static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu);
    }
}

bool is_srgb_transfer(VkFormat fmt) {
    switch (fmt) {
    case VkFormat::bc1_rgb_srgb_block:
    case VkFormat::bc3_srgb_block:
    case VkFormat::bc7_srgb_block:
    case VkFormat::etc2_r8g8b8_srgb_block:
    case VkFormat::astc_4x4_srgb_block:
    case VkFormat::astc_5x5_srgb_block:
    case VkFormat::astc_6x6_srgb_block:
    case VkFormat::astc_8x8_srgb_block:
        return true;
    case VkFormat::bc1_rgb_unorm_block:
    case VkFormat::bc3_unorm_block:
    case VkFormat::bc7_unorm_block:
    case VkFormat::etc2_r8g8b8_unorm_block:
    default:
        return false;
    }
}

std::uint32_t color_model_for(VkFormat fmt) {
    switch (fmt) {
    case VkFormat::bc1_rgb_srgb_block:
    case VkFormat::bc1_rgb_unorm_block:
        return kModelBc1a;
    case VkFormat::bc3_srgb_block:
    case VkFormat::bc3_unorm_block:
        return kModelBc3;
    case VkFormat::bc7_srgb_block:
    case VkFormat::bc7_unorm_block:
        return kModelBc7;
    case VkFormat::etc2_r8g8b8_srgb_block:
    case VkFormat::etc2_r8g8b8_unorm_block:
        return kModelEtc2;
    case VkFormat::astc_4x4_srgb_block:
    case VkFormat::astc_5x5_srgb_block:
    case VkFormat::astc_6x6_srgb_block:
    case VkFormat::astc_8x8_srgb_block:
        return kModelAstc;
    default:
        return kModelEtc2;
    }
}

std::uint8_t channel_type_for(VkFormat fmt) {
    switch (fmt) {
    case VkFormat::bc1_rgb_srgb_block:
    case VkFormat::bc1_rgb_unorm_block:
        return kBc1ChannelColor;
    case VkFormat::bc3_srgb_block:
    case VkFormat::bc3_unorm_block:
        return kBc3ChannelColor;
    case VkFormat::bc7_srgb_block:
    case VkFormat::bc7_unorm_block:
        return kBc7ChannelColor;
    default:
        return kEtc2ChannelColor;
    }
}

// Build the basic Data Format Descriptor (DFD) for a single-sample RGB
// compressed block format. Layout (KDFS §3):
//
//   u32 totalSize         — DFD total size in bytes (including this field)
//   block header (24 B):
//     u16 vendorId | u16 (descriptorType<<? not separate field) — actually
//        vendorId is bits[16:0], descriptorType bits[31:16]. We pack as u32.
//     u16 versionNumber
//     u16 descriptorBlockSize
//     u8  colorModel
//     u8  colorPrimaries
//     u8  transferFunction
//     u8  flags
//     u8  texelBlockDimension[4]   — values are (dim - 1)
//     u8  bytesPlane[8]            — first byte = bytes per block; rest 0
//   per sample (16 B each):
//     u16 bitOffset
//     u8  bitLength                — value is (length - 1)
//     u8  channelType+qualifiers   — high nibble = qualifiers, low = type
//     u8  samplePosition[4]
//     u32 sampleLower
//     u32 sampleUpper
std::vector<std::uint8_t> build_dfd(const Inputs& in) {
    constexpr std::uint16_t kSampleSize = 16;
    constexpr std::uint16_t kBlockHeaderSize = 24;
    constexpr std::uint16_t kSamples = 1;
    constexpr std::uint16_t kBlockSize = kBlockHeaderSize + kSampleSize * kSamples;
    std::uint32_t total = 4 + kBlockSize;

    std::vector<std::uint8_t> dfd;
    dfd.reserve(total);
    put_u32(dfd, total);

    // Block header
    // vendorId (low 17 bits) + descriptorType (high 15 bits). For Khronos
    // basic format descriptor both are 0.
    put_u16(dfd, 0);                                          // vendorId low
    put_u16(dfd, 0);                                          // descriptorType
    put_u16(dfd, 2);                                          // versionNumber = 2
    put_u16(dfd, kBlockSize);                                 // descriptorBlockSize
    dfd.push_back(static_cast<std::uint8_t>(color_model_for(in.format)));
    dfd.push_back(static_cast<std::uint8_t>(kPrimariesBT709));
    dfd.push_back(static_cast<std::uint8_t>(is_srgb_transfer(in.format) ? kTransferSrgb
                                                                        : kTransferLinear));
    dfd.push_back(0);  // flags — straight alpha, no premul (no alpha here)
    // texelBlockDimension: values are (dim - 1); unused dims are 0.
    dfd.push_back(static_cast<std::uint8_t>(in.block_dim.w - 1));
    dfd.push_back(static_cast<std::uint8_t>(in.block_dim.h - 1));
    dfd.push_back(static_cast<std::uint8_t>(in.block_dim.d - 1));
    dfd.push_back(0);  // 4D unused
    // bytesPlane: first byte = bytes per block; rest 0 (single plane).
    dfd.push_back(static_cast<std::uint8_t>(in.bytes_per_block));
    for (int i = 0; i < 7; ++i) dfd.push_back(0);

    // Sample 0 — single sample covering all RGB bits of the block.
    put_u16(dfd, 0);                                  // bitOffset = 0
    dfd.push_back(static_cast<std::uint8_t>(in.bytes_per_block * 8 - 1));  // bitLength - 1
    dfd.push_back(channel_type_for(in.format));       // channelType + qualifiers
    dfd.push_back(0);                                 // samplePosition[0]
    dfd.push_back(0);
    dfd.push_back(0);
    dfd.push_back(0);
    put_u32(dfd, 0);                                  // sampleLower
    put_u32(dfd, 0xFFFFFFFFu);                        // sampleUpper

    return dfd;
}

}  // namespace

std::vector<std::uint8_t> write(const Inputs& in) {
    std::vector<std::uint8_t> out;
    constexpr std::size_t kHeaderSize = 80;
    constexpr std::size_t kLevelIndexEntrySize = 24;

    auto dfd = build_dfd(in);

    // Collect levels: prefer in.levels if non-empty, otherwise wrap block_bytes
    // as a single level. levels[0] = base (largest), levels[i+1] is the next
    // smaller mip.
    std::vector<std::span<const std::uint8_t>> level_inputs;
    if (!in.levels.empty()) {
        for (const auto& lv : in.levels) {
            level_inputs.emplace_back(lv);
        }
    } else {
        level_inputs.emplace_back(in.block_bytes);
    }
    const std::size_t level_count = level_inputs.size();

    // Per-level: optional zstd compression (lossless). Each level is compressed
    // independently so partial loads stay possible.
    std::vector<std::vector<std::uint8_t>> compressed_levels(level_count);
    std::vector<const std::uint8_t*> level_data_ptr(level_count);
    std::vector<std::size_t> level_data_size(level_count);
    std::vector<std::size_t> level_uncompressed_size(level_count);
    for (std::size_t i = 0; i < level_count; ++i) {
        level_uncompressed_size[i] = level_inputs[i].size();
        if (in.supercompress_zstd && !level_inputs[i].empty()) {
            std::size_t bound = ZSTD_compressBound(level_inputs[i].size());
            compressed_levels[i].resize(bound);
            std::size_t written = ZSTD_compress(compressed_levels[i].data(), bound,
                                                level_inputs[i].data(),
                                                level_inputs[i].size(),
                                                19);
            if (!ZSTD_isError(written)) {
                compressed_levels[i].resize(written);
                level_data_ptr[i] = compressed_levels[i].data();
                level_data_size[i] = compressed_levels[i].size();
                continue;
            }
        }
        level_data_ptr[i] = level_inputs[i].data();
        level_data_size[i] = level_inputs[i].size();
    }

    std::size_t total_data = 0;
    for (auto s : level_data_size) total_data += s;
    std::size_t total = kHeaderSize + kLevelIndexEntrySize * level_count +
                        dfd.size() + total_data;
    out.reserve(total);

    // --- Header (80 bytes) -------------------------------------------------
    for (std::uint8_t c : kIdentifier) out.push_back(c);
    put_u32(out, static_cast<std::uint32_t>(in.format));
    put_u32(out, 1);                                      // typeSize
    put_u32(out, static_cast<std::uint32_t>(in.image_w));
    put_u32(out, static_cast<std::uint32_t>(in.image_h));
    put_u32(out, 0);                                      // pixelDepth
    put_u32(out, 0);                                      // layerCount
    put_u32(out, 1);                                      // faceCount
    put_u32(out, static_cast<std::uint32_t>(level_count));
    put_u32(out, in.supercompress_zstd ? 2u : 0u);
    std::size_t dfd_offset_pos = out.size();
    put_u32(out, 0);
    put_u32(out, static_cast<std::uint32_t>(dfd.size()));
    std::size_t kvd_offset_pos = out.size();
    put_u32(out, 0);
    put_u32(out, 0);
    put_u64(out, 0);
    put_u64(out, 0);

    // --- Level index --- (one 24-byte entry per level, indexed level[0..N-1]
    // = level 0..N-1 in natural numbering; byteOffsets patched after we
    // know the actual file layout)
    std::vector<std::size_t> level_byte_offset_pos(level_count);
    for (std::size_t i = 0; i < level_count; ++i) {
        level_byte_offset_pos[i] = out.size();
        put_u64(out, 0);                                                   // byteOffset (patched)
        put_u64(out, static_cast<std::uint64_t>(level_data_size[i]));      // byteLength
        put_u64(out, static_cast<std::uint64_t>(level_uncompressed_size[i]));
    }

    // --- DFD ---------------------------------------------------------------
    std::uint32_t dfd_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), dfd.begin(), dfd.end());
    std::uint32_t kvd_offset = 0;

    // --- Mip level data ----------------------------------------------------
    // KTX2 §3.10.4: levels stored smallest-first so base level lies at the
    // file's end. levelIndex[i] still points at level i in natural numbering;
    // we patch each entry's byteOffset to the actual file location.
    while ((out.size() & 7u) != 0u) out.push_back(0);

    std::vector<std::uint64_t> level_offset(level_count);
    for (std::size_t i = level_count; i-- > 0;) {
        // 8-byte align before each level (libktx convention).
        while ((out.size() & 7u) != 0u) out.push_back(0);
        level_offset[i] = out.size();
        if (level_data_size[i] > 0) {
            out.insert(out.end(), level_data_ptr[i],
                       level_data_ptr[i] + level_data_size[i]);
        }
    }

    // --- Patch back-references --------------------------------------------
    write_u32_at(out, dfd_offset_pos, dfd_offset);
    write_u32_at(out, kvd_offset_pos, kvd_offset);
    for (std::size_t i = 0; i < level_count; ++i) {
        write_u64_at(out, level_byte_offset_pos[i], level_offset[i]);
    }
    return out;
}

}  // namespace png2amiga::ktx2
