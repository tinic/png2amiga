#!/usr/bin/env python3
# Enumerate valid 2-partition CEM-8 bilinear-decim candidates per footprint.
# 2p bit layout (see pack_block_2p_decim): 29 bits overhead
# (block_mode 11 + partition_count 2 + partition_index 10 + CEM 6), then
# 12 endpoint values (2 partitions x 2 endpoints x 3ch) at the auto-picked
# quant, then weights from the top. So endpoint budget = 99 - weight_bits,
# and the decoder picks the highest-value quant level whose 12-value ISE
# size fits the budget. The 2p pack only supports power-of-2 weight quants
# (Q2/Q4/Q8/Q16/Q32 straight binary).

def decode(block_mode):
    base_quant_mode = (block_mode >> 4) & 1
    H = (block_mode >> 9) & 1
    D = (block_mode >> 10) & 1
    A = (block_mode >> 5) & 0x3
    x_weights = 0; y_weights = 0
    if (block_mode & 3) != 0:
        base_quant_mode |= (block_mode & 3) << 1
        B = (block_mode >> 7) & 3
        scheme = (block_mode >> 2) & 3
        if scheme == 0: x_weights = B + 4; y_weights = A + 2
        elif scheme == 1: x_weights = B + 8; y_weights = A + 2
        elif scheme == 2: x_weights = A + 2; y_weights = B + 8
        elif scheme == 3:
            B &= 1
            if block_mode & 0x100: x_weights = B + 2; y_weights = A + 2
            else: x_weights = A + 2; y_weights = B + 6
    else:
        base_quant_mode |= ((block_mode >> 2) & 3) << 1
        if ((block_mode >> 2) & 3) == 0: return None
        B = (block_mode >> 9) & 3
        scheme = (block_mode >> 7) & 3
        if scheme == 0: x_weights = 12; y_weights = A + 2
        elif scheme == 1: x_weights = A + 2; y_weights = 12
        elif scheme == 2: x_weights = A + 6; y_weights = B + 6; D = 0; H = 0
        elif scheme == 3:
            sub = (block_mode >> 5) & 3
            if sub == 0: x_weights = 6; y_weights = 10
            elif sub == 1: x_weights = 10; y_weights = 6
            else: return None
    weight_count = x_weights * y_weights * (D + 1)
    quant_mode = (base_quant_mode - 2) + 6 * H
    if quant_mode < 0 or quant_mode > 11: return None
    if weight_count < 1 or weight_count > 64: return None
    return (x_weights, y_weights, D, quant_mode)

# weight quant index -> value; only power-of-2 are usable by the 2p pack.
QN = {0:2, 1:3, 2:4, 3:5, 4:6, 5:8, 6:10, 7:12, 8:16, 9:20, 10:24, 11:32}
POW2_QMODE = {2: 0, 4: 2, 8: 5, 16: 8, 32: 11}  # value -> weight qmode
WBITS = {2: 1, 4: 2, 8: 3, 16: 4, 32: 5}         # value -> bits per weight

# Endpoint levels supported by EPQuantOps + their 12-value ISE bit size.
def ep_bits12(level):
    binbits = {4:2, 8:3, 16:4, 32:5, 64:6, 128:7, 256:8}
    tritd  = {3:0, 6:1, 12:2, 24:3, 48:4, 96:5, 192:6}
    quintd = {5:0, 10:1, 20:2, 40:3, 80:4, 160:5}
    if level in binbits: return 12 * binbits[level]
    if level in tritd:   return 12 * tritd[level] + 20   # (12*8+4)//5
    if level in quintd:  return 12 * quintd[level] + 28   # (12*7+2)//3
    raise ValueError(level)

# Levels with an EPQuantOps specialization in astc.cpp, finest -> coarsest.
EP_LEVELS = [256, 192, 160, 128, 96, 80, 64, 48, 40, 32, 24]

def pick_epq(weight_bits):
    budget = 99 - weight_bits
    for lv in EP_LEVELS:
        if ep_bits12(lv) <= budget:
            return lv
    return None

# enumerate (gw, gh, weight_qmode) -> min block_mode
modes = {}
for bm in range(2048):
    r = decode(bm)
    if r is None: continue
    xw, yw, D, qm = r
    if D != 0: continue
    key = (xw, yw, qm)
    if key not in modes or bm < modes[key]:
        modes[key] = bm

footprints = [(5,5),(6,5),(6,6),(8,5),(8,6),(8,8),
              (10,5),(10,6),(10,8),(10,10),(12,10),(12,12)]

EPQ_FLOOR = 24
import os as _os
# Endpoint quant is capped at Q64: levels >= Q80 at n=12 endpoints (only
# reachable from tiny 2x2/2x3 grids) produce a decode mismatch — our
# predicted decode diverges from the real decoder, so the candidate wins
# the OKLab^2 argmin with a bogus-low err and paints garbage (-10 S2 from
# a single such candidate, -97 from the full uncapped set). The originals
# only ever used Q24/Q40/Q64 at n=12, all validated. Coarser endpoints
# (the useful finer-grid 2p candidates) stay well within Q64.
EPQ_CAP = int(_os.environ.get('ENUM2P_EPCAP', '64'))

import os
# LIMIT modes for bisection:
#   LIMIT=orig  -> emit only the original 4 candidates (verify refactor)
#   LIMIT=N     -> original 4 + first N enumerated candidates (sorted)
LIMIT = os.environ.get('ENUM2P_LIMIT', '')
ORIG = [(4,4,4,0x042,32,40), (5,4,4,0x0C2,40,24),
        (3,3,8,0x1BF,27,64), (4,3,4,0x022,24,64)]

for (fw, fh) in footprints:
    cands = []
    for wlval, qm in POW2_QMODE.items():
        for (gw, gh, q), bm in modes.items():
            if q != qm: continue
            if gw > fw or gh > fh: continue
            gn = gw * gh
            wb = gn * WBITS[wlval]
            epq = pick_epq(wb)
            if epq is None or epq < EPQ_FLOOR: continue
            if epq > EPQ_CAP: continue
            cands.append((gw, gh, wlval, bm, wb, epq))
    cands.sort()
    if LIMIT == 'orig':
        cands = [c for c in ORIG if c[0] <= fw and c[1] <= fh]
    elif LIMIT.isdigit():
        keep = [c for c in ORIG if c[0] <= fw and c[1] <= fh]
        for c in cands:
            if len(keep) >= len(ORIG) + int(LIMIT): break
            if c not in keep: keep.append(c)
        cands = keep
    items = [f"DecimCfg2p{{{gw},{gh},0x{bm:03X},{wl},{epq}}}"
             for gw, gh, wl, bm, wb, epq in cands]
    print(f"// {fw}x{fh}: {len(cands)} 2p candidates")
    print(f"<<<{fw}x{fh}>>>")
    print("                    " + ",\n                    ".join(items))
    print()
