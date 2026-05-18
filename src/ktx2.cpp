// KTX2 container writer — see ktx2.hpp for layout.
//
// Spec: https://github.khronos.org/KTX-Specification/ — appendix tables
// for Khronos Data Format Specification (KDFS) color models / transfer
// functions / channel types.

#include "ktx2.hpp"

#include <cstring>

namespace png2amiga::ktx2 {

namespace {

constexpr std::uint8_t kIdentifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};

// KDFS color-model IDs (KDFS v1.3 §15.1).
// KHR_DF_MODEL_* values from khr_df.h.
constexpr std::uint32_t kModelBc1a = 128;
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
    case VkFormat::etc2_r8g8b8_srgb_block:
    case VkFormat::astc_4x4_srgb_block:
    case VkFormat::astc_5x5_srgb_block:
    case VkFormat::astc_6x6_srgb_block:
    case VkFormat::astc_8x8_srgb_block:
        return true;
    case VkFormat::bc1_rgb_unorm_block:
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

    // Reserve once for everything to avoid reallocation; we'll back-patch
    // the byte offsets into the header after the levels' bytes land.
    std::size_t image_data_size = in.block_bytes.size();
    std::size_t total = kHeaderSize + kLevelIndexEntrySize + dfd.size() + image_data_size;
    out.reserve(total);

    // --- Header (80 bytes) -------------------------------------------------
    for (std::uint8_t c : kIdentifier) out.push_back(c);
    put_u32(out, static_cast<std::uint32_t>(in.format));  // vkFormat
    put_u32(out, 1);                                      // typeSize (compressed block formats use 1)
    put_u32(out, static_cast<std::uint32_t>(in.image_w));
    put_u32(out, static_cast<std::uint32_t>(in.image_h));
    put_u32(out, 0);                                      // pixelDepth = 0 for 2D
    put_u32(out, 0);                                      // layerCount = 0 for non-array
    put_u32(out, 1);                                      // faceCount = 1
    put_u32(out, 1);                                      // levelCount = 1
    put_u32(out, 0);                                      // supercompressionScheme = none
    std::size_t dfd_offset_pos = out.size();
    put_u32(out, 0);                                      // dfdByteOffset (patched)
    put_u32(out, static_cast<std::uint32_t>(dfd.size())); // dfdByteLength
    std::size_t kvd_offset_pos = out.size();
    put_u32(out, 0);                                      // kvdByteOffset (patched)
    put_u32(out, 0);                                      // kvdByteLength (empty)
    put_u64(out, 0);                                      // sgdByteOffset
    put_u64(out, 0);                                      // sgdByteLength

    // --- Level index (24 bytes per level, 1 level) -------------------------
    std::size_t level_byte_offset_pos = out.size();
    put_u64(out, 0);                                      // byteOffset (patched)
    put_u64(out, static_cast<std::uint64_t>(image_data_size));  // byteLength
    put_u64(out, static_cast<std::uint64_t>(image_data_size));  // uncompressedByteLength
                                                                // (no supercompression → equal)

    // --- DFD ---------------------------------------------------------------
    std::uint32_t dfd_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), dfd.begin(), dfd.end());

    // --- KVD ---------------------------------------------------------------
    // Per KTX2 spec: when kvdByteLength == 0, kvdByteOffset MUST be 0
    // (not the position-after-DFD). The Khronos `ktx validate` flags this.
    std::uint32_t kvd_offset = 0;

    // --- Mip level data ----------------------------------------------------
    // KTX2 spec requires level data to be 8-byte-aligned relative to file
    // start (for the largest scalar in any vkFormat). Compressed-block
    // formats have typeSize=1 so alignment is 1, but tools (libktx) prefer
    // 8-byte for portability. We pad here.
    while ((out.size() & 7u) != 0u) out.push_back(0);
    std::uint64_t level_offset = out.size();

    out.insert(out.end(), in.block_bytes.begin(), in.block_bytes.end());

    // --- Patch the back-references in the header --------------------------
    write_u32_at(out, dfd_offset_pos, dfd_offset);
    write_u32_at(out, kvd_offset_pos, kvd_offset);
    write_u64_at(out, level_byte_offset_pos, level_offset);

    return out;
}

}  // namespace png2amiga::ktx2
