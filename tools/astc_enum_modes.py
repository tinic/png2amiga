#!/usr/bin/env python3
"""ASTC block-mode enumerator + dispatcher codegen.

Port of decode_block_mode_2d from astcenc_block_sizes.cpp. Walks all
2048 possible 11-bit block_mode values, decodes each to (grid_w, grid_h,
weight_quant, dual_plane), computes weight_bits via the BISE formula,
and prints per-footprint candidate lists for src/astc.cpp's exhaustive
per-block dispatcher.

Usage:
  tools/astc_enum_modes.py        # print dispatcher source for all footprints
  tools/astc_enum_modes.py table  # print per-grid finest viable quant
"""
import sys


def ise_bits(count, qm):
    """Bits used by BISE encoding of `count` values at quant_mode `qm`."""
    # quant_mode index → quant level:
    #   0=Q2 1=Q3 2=Q4 3=Q5 4=Q6 5=Q8 6=Q10 7=Q12 8=Q16 9=Q20 10=Q24 11=Q32
    if qm in (0, 2, 5, 8, 11):  # straight binary
        bpw = {0: 1, 2: 2, 5: 3, 8: 4, 11: 5}[qm]
        return count * bpw
    if qm in (1, 4, 7, 10):  # trit-packed
        bits = (count * 8 + 4) // 5
        bits += count * {1: 0, 4: 1, 7: 2, 10: 3}[qm]
        return bits
    if qm in (3, 6, 9):  # quint-packed
        bits = (count * 7 + 2) // 3
        bits += count * {3: 0, 6: 1, 9: 2}[qm]
        return bits
    return 999


def decode(block_mode):
    """Decode 11-bit block_mode to (x_weights, y_weights, D, qmode, weight_bits)
    or None if the mode is invalid for 2D LDR blocks.
    """
    base_quant_mode = (block_mode >> 4) & 1
    H = (block_mode >> 9) & 1
    D = (block_mode >> 10) & 1
    A = (block_mode >> 5) & 0x3
    x_weights = 0
    y_weights = 0
    if (block_mode & 3) != 0:
        base_quant_mode |= (block_mode & 3) << 1
        B = (block_mode >> 7) & 3
        scheme = (block_mode >> 2) & 3
        if scheme == 0:
            x_weights = B + 4
            y_weights = A + 2
        elif scheme == 1:
            x_weights = B + 8
            y_weights = A + 2
        elif scheme == 2:
            x_weights = A + 2
            y_weights = B + 8
        elif scheme == 3:
            B &= 1
            if block_mode & 0x100:
                x_weights = B + 2
                y_weights = A + 2
            else:
                x_weights = A + 2
                y_weights = B + 6
    else:
        base_quant_mode |= ((block_mode >> 2) & 3) << 1
        if ((block_mode >> 2) & 3) == 0:
            return None
        B = (block_mode >> 9) & 3
        scheme = (block_mode >> 7) & 3
        if scheme == 0:
            x_weights = 12
            y_weights = A + 2
        elif scheme == 1:
            x_weights = A + 2
            y_weights = 12
        elif scheme == 2:
            x_weights = A + 6
            y_weights = B + 6
            D = 0
            H = 0
        elif scheme == 3:
            sub = (block_mode >> 5) & 3
            if sub == 0:
                x_weights = 6
                y_weights = 10
            elif sub == 1:
                x_weights = 10
                y_weights = 6
            else:
                return None
    weight_count = x_weights * y_weights * (D + 1)
    quant_mode = (base_quant_mode - 2) + 6 * H
    if quant_mode < 0 or quant_mode > 11:
        return None
    if weight_count < 1 or weight_count > 64:
        return None
    wb = ise_bits(weight_count, quant_mode)
    if wb < 24 or wb > 96:
        return None
    return (x_weights, y_weights, D, quant_mode, wb)


QN = {0: 2, 1: 3, 2: 4, 3: 5, 4: 6, 5: 8, 6: 10, 7: 12, 8: 16, 9: 20, 10: 24, 11: 32}

# Footprints png2amiga ships
FOOTPRINTS = [(4, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8),
              (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]


def best_per_grid():
    """For each (xw, yw), pick the finest weight_quant that fits 63 bits.

    CEM-8 single-partition with QUANT_256 endpoints needs 17 bits header
    + 48 bits endpoints = 65 bits, leaving ≤ 63 bits for weights.
    """
    result = {}
    for bm in range(2048):
        r = decode(bm)
        if r is None:
            continue
        xw, yw, D, qm, wb = r
        if D != 0 or wb > 63:
            continue
        cur = result.get((xw, yw))
        if cur is None or qm > cur[0]:
            result[(xw, yw)] = (qm, wb, bm)
    return result


def print_table():
    for (xw, yw), (qm, wb, bm) in sorted(best_per_grid().items()):
        print(f"  {xw}x{yw}: Q{QN[qm]} = {wb}b, block_mode = 0x{bm:03X}")


def print_dispatchers():
    grids = best_per_grid()
    for (fw, fh) in FOOTPRINTS:
        cands = []
        for (xw, yw), (qm, wb, bm) in sorted(grids.items()):
            if xw <= fw and yw <= fh:
                cands.append((xw, yw, qm, bm))
        n = len(cands)
        items = [f"DecimCfg{{{xw},{yw},0x{bm:03X},{QN[qm]}}}"
                 for xw, yw, qm, bm in cands]
        lines = []
        for i in range(0, len(items), 2):
            lines.append(", ".join(items[i:i + 2]))
        body = ",\n                    ".join(lines)
        print(f"        // {fw}x{fh}: {n} candidates")
        print(f"        if (W == {fw} && H == {fh})")
        print(f"            return encode_image_impl<{fw}, {fh}>"
              f"(rgba_srgb8, image_w, image_h, options,")
        print(f"                make_encode_fn_decim_pack<{fw}, {fh}, M,")
        print(f"                    {body}>());")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "table":
        print_table()
    else:
        print_dispatchers()
