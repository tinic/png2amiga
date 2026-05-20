def ise_bits(count, qm):
    if qm in (0,2,5,8,11):
        bpw = {0:1, 2:2, 5:3, 8:4, 11:5}[qm]
        return count * bpw
    if qm in (1,4,7,10):
        bits = (count * 8 + 4) // 5
        bits += count * {1:0,4:1,7:2,10:3}[qm]
        return bits
    if qm in (3,6,9):
        bits = (count * 7 + 2) // 3
        bits += count * {3:0,6:1,9:2}[qm]
        return bits
    return 999

def decode(block_mode):
    base_quant_mode = (block_mode >> 4) & 1
    H = (block_mode >> 9) & 1
    D = (block_mode >> 10) & 1
    A = (block_mode >> 5) & 0x3
    x_weights=0; y_weights=0
    if (block_mode & 3) != 0:
        base_quant_mode |= (block_mode & 3) << 1
        B = (block_mode >> 7) & 3
        scheme = (block_mode >> 2) & 3
        if scheme == 0: x_weights=B+4; y_weights=A+2
        elif scheme == 1: x_weights=B+8; y_weights=A+2
        elif scheme == 2: x_weights=A+2; y_weights=B+8
        elif scheme == 3:
            B &= 1
            if block_mode & 0x100: x_weights=B+2; y_weights=A+2
            else: x_weights=A+2; y_weights=B+6
    else:
        base_quant_mode |= ((block_mode >> 2) & 3) << 1
        if ((block_mode >> 2) & 3) == 0: return None
        B = (block_mode >> 9) & 3
        scheme = (block_mode >> 7) & 3
        if scheme == 0: x_weights=12; y_weights=A+2
        elif scheme == 1: x_weights=A+2; y_weights=12
        elif scheme == 2: x_weights=A+6; y_weights=B+6; D=0; H=0
        elif scheme == 3:
            sub = (block_mode >> 5) & 3
            if sub == 0: x_weights=6; y_weights=10
            elif sub == 1: x_weights=10; y_weights=6
            else: return None
    weight_count = x_weights * y_weights * (D + 1)
    quant_mode = (base_quant_mode - 2) + 6 * H
    if quant_mode < 0 or quant_mode > 11: return None
    if weight_count < 1 or weight_count > 64: return None
    wb = ise_bits(weight_count, quant_mode)
    if wb < 24 or wb > 96: return None
    return (x_weights, y_weights, D, quant_mode, wb)

QN = {0:2,1:3,2:4,3:5,4:6,5:8,6:10,7:12,8:16,9:20,10:24,11:32}

# endpoint bits for 6 values per EPQuant level (CEM-8 1p)
def ep_bits6(epq_level):
    # epq_level: bits per endpoint at QUANT — use ise on 6 values
    return ise_bits(6, epq_level)
# Map quant_mode index to EPQuant value for endpoints.
# Endpoint quant levels we support EPQuantOps for: 256,192,160,128,96,80,64,48,40,32,24
# Their (value, ise-qmode) — ise uses the same trit/quint/binary logic.
# For 6 endpoints, bits:
EPQ_BITS = {256:48, 192:46, 160:44, 128:42, 96:40, 80:38, 64:36, 48:34, 40:32, 32:30, 24:28}
EPQ_LEVELS = sorted(EPQ_BITS.keys(), reverse=True)  # high→low

def pick_epq(weight_bits):
    budget = 128 - 17 - weight_bits
    for epq in EPQ_LEVELS:  # highest first
        if EPQ_BITS[epq] <= budget:
            return epq
    return None

# enumerate all valid block modes → (grid, qmode, wb, bm)
modes = {}  # (gw, gh, qmode) -> (wb, bm)
for bm in range(2048):
    r = decode(bm)
    if r is None: continue
    xw, yw, D, qm, wb = r
    if D != 0: continue
    key = (xw, yw, qm)
    if key not in modes or wb < modes[key][0]:
        modes[key] = (wb, bm)

footprints = [(5,5),(6,5),(6,6),(8,5),(8,6),(8,8),(10,5),(10,6),(10,8),(10,10),(12,10),(12,12)]

# For each footprint: candidates with weight_bits in [64, 75] (tighter EPQ band,
# EPQuant in [Q64, Q192]; below 64 already in Q256 pack; above 75 too coarse).
for (fw, fh) in footprints:
    cands = []
    seen = set()
    for (gw, gh, qm), (wb, bm) in modes.items():
        if gw > fw or gh > fh: continue
        if wb < 64 or wb > 75: continue
        epq = pick_epq(wb)
        if epq is None or epq < 64: continue
        # dedup by (gw, gh, qm)
        k = (gw, gh, qm)
        if k in seen: continue
        seen.add(k)
        cands.append((gw, gh, QN[qm], bm, wb, epq))
    cands.sort()
    items = [f"DecimCfg{{{gw},{gh},0x{bm:03X},{q}, 8, {epq}}}" for gw,gh,q,bm,wb,epq in cands]
    body = ",\n                    ".join(items)
    print(f"// {fw}x{fh}: {len(cands)} tighter-EPQ candidates")
    print(f"<<<{fw}x{fh}>>>")
    print(f"                    {body}")
    print()
