#pragma once
#include "amiga.hpp"
#include "types.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace png2amiga::thomson::k7 {
// TO cassette blocks: FF leader, 01 3C, type, payload length, payload,
// additive checksum of type + length + payload (modulo 256).
// Reference: https://github.com/spotlessmind1975/ugbasic/blob/main/ugbc/src/targets/to8/_cleanup.c
// (addFileEntry/addFileData describe the TO-specific wire format).
inline void block(std::vector<std::uint8_t>& out, std::uint8_t type,
                  std::span<const std::uint8_t> data) {
    out.insert(out.end(), 16, 0xff);
    out.insert(out.end(), {0x01, 0x3c, type, static_cast<std::uint8_t>(data.size())});
    std::uint8_t sum = static_cast<std::uint8_t>(type + data.size());
    for (auto b : data) { out.push_back(b); sum += b; }
    out.push_back(sum);
}

inline Result<std::vector<std::uint8_t>> encode(amiga::Mode mode,
                                               std::span<const std::uint8_t> frame) {
    if (mode != amiga::Mode::thomson_to7_320x16)
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            "K7 export requires thomson-to7-320x16 (TO7/70) mode"}};
    if (frame.size() != 16000)
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "TO7/70 K7 export requires a 16000-byte couleur/forme frame"}};

    // LOADM loads at $8000, above BASIC 1's workspace. The image ends
    // at $BEC0, leaving room below BASIC's stack and tape buffer near $DF00.
    // The TO7/70's default RAM mapping holds the viewer and both planes.
    // $E7C3 bit 0 selects couleur (0) / forme (1).
    // Disable IRQ/FIRQ so the BASIC cursor cannot damage the displayed image.
    // Reset exits the viewer. The original 8-color TO7 is not supported.
    std::vector<std::uint8_t> program{
        0x1a,0x50,                    // ORCC #$50
        0xb6,0xe7,0xc3, 0x84,0xfe,   // LDA $E7C3 / ANDA #$FE
        0xb7,0xe7,0xc3,               // STA $E7C3: couleur
        0x8e,0x80,0x40,               // LDX #$8040
        0x10,0x8e,0x40,0x00,          // LDY #$4000
        0xce,0x1f,0x40,               // LDU #8000
        0xa6,0x80, 0xa7,0xa0,         // LDA ,X+ / STA ,Y+
        0x33,0x5f,                    // LEAU -1,U (does not set Z)
        0x11,0x83,0x00,0x00,         // CMPU #0
        0x26,0xf4,                    // BNE loop
        0xb6,0xe7,0xc3, 0x8a,0x01,   // LDA $E7C3 / ORA #1
        0xb7,0xe7,0xc3,               // STA $E7C3: forme
        0x10,0x8e,0x40,0x00,          // LDY #$4000
        0xce,0x1f,0x40,               // LDU #8000
        0xa6,0x80, 0xa7,0xa0,
        0x33,0x5f, 0x11,0x83,0x00,0x00, 0x26,0xf4,
        0x20,0xfe                     // BRA *
    };
    program.resize(64, 0x12);         // NOP padding; data at $8040
    program.insert(program.end(), frame.begin(), frame.end());
    const auto n = program.size();
    std::vector<std::uint8_t> binary{0x00, static_cast<std::uint8_t>(n >> 8),
        static_cast<std::uint8_t>(n), 0x80, 0x00};
    binary.insert(binary.end(), program.begin(), program.end());
    binary.insert(binary.end(), {0xff,0x00,0x00,0x80,0x00}); // execution record
    const std::array<std::uint8_t,20> header{
        'P','I','C','T','U','R','E',' ', 'B','I','N',
        2,0,0, 0,0,0,0,0,0}; // binary, no inter-block motor stops
    std::vector<std::uint8_t> tape;
    block(tape, 0, header);
    tape.insert(tape.end(), 150, 0xff); // header/data motor restart leader
    for (std::size_t p = 0; p < binary.size(); p += 255)
        block(tape, 1, std::span(binary).subspan(p, std::min<std::size_t>(255, binary.size()-p)));
    block(tape, 0xff, {});
    return tape;
}
} // namespace png2amiga::thomson::k7
