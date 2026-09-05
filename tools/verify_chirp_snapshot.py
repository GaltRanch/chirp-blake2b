#!/usr/bin/env python3
"""Verify a CHIRP registry-snapshot commitment.

A CHIRP block commits SHA256 of the exact registry snapshot it used into its own
coinbase (an OP_RETURN output: 0x6a <push37> "CHRP1" || hash32). The pool publishes
that snapshot as a file named <hash>.chirpsnap. This tool:

  1. hashes the snapshot file and checks it matches the commitment
     (either --expect-hash HEX, or the OP_RETURN parsed from --coinbase-hex);
  2. optionally re-runs the whole weighted draw + coinbase split from the snapshot
     + the previous block hash (the seed) + the CHIRP params, so you can compare the
     result against the block's actual coinbase outputs.

Because the snapshot is committed on-chain, the pool is bound to the tenure/power
figures it used and cannot rewrite them after the fact. (What this still trusts: that
the pool didn't inflate a favoured address in real time BEFORE committing — closed
further by signed share logs / DATUM-TS. See docs/chirp_snapshot_commitment.md.)

Python 3.8+, standard library only.
"""
import argparse, hashlib, sys

U64 = (1 << 64) - 1

# ---- draw math: exact ports of src/datum_chirp.c ----

def u01(seed, addr):
    """FNV-1a(seed ^ addr bytes) → splitmix64 → (0,1]. Matches chirp_u01()."""
    h = (0xcbf29ce484222325 ^ seed) & U64
    for b in addr.encode():
        h = ((h ^ b) * 0x00000100000001b3) & U64
    h = (h ^ (h >> 30)) & U64
    h = (h * 0xbf58476d1ce4e5b9) & U64
    h = (h ^ (h >> 27)) & U64
    h = (h * 0x94d049bb133111eb) & U64
    h = (h ^ (h >> 31)) & U64
    v = h / float(U64)
    if v < 1e-12: v = 1e-12
    if v > 1.0:   v = 1.0
    return v

def weighted_draw(cands, seed, n):
    """Efraimidis–Spirakis without replacement: key = u^(1/w), top n. Matches chirp_weighted_draw()."""
    keyed = []
    for c in cands:
        if c['weight'] <= 0.0:
            continue
        key = u01(seed, c['addr']) ** (1.0 / c['weight'])
        keyed.append((key, c))
    keyed.sort(key=lambda kc: kc[0], reverse=True)
    return [c for _, c in keyed[:n]]

def split(cands, total_value, fee_bps, seed, max_n, min_payout):
    """Matches chirp_split(): fee off the top, rest ∝ weight among the drawn winners, floor rounding."""
    base_fee = (total_value * fee_bps) // 10000
    distributable = max(0, total_value - base_fee)
    winners = weighted_draw(cands, seed, max_n)
    total_w = sum(w['weight'] for w in winners)
    payouts = []
    if not winners or total_w <= 0:
        return payouts, total_value
    assigned = 0
    for w in winners:
        amt = int(distributable * (w['weight'] / total_w))  # floor, like the C double→uint64 cast
        if amt >= min_payout:
            payouts.append({'addr': w['addr'], 'sats': amt})
            assigned += amt
    return payouts, max(0, total_value - assigned)

# ---- snapshot parsing ----

def parse_snapshot(raw):
    """Parse canonical CHIRPSNAP1 bytes → (snapshot_time, [ {addr, tenure_secs, power} ])."""
    lines = raw.decode('utf-8').split('\n')
    if not lines or lines[0] != 'CHIRPSNAP1':
        raise ValueError('not a CHIRPSNAP1 snapshot (bad magic)')
    if len(lines) < 2 or not lines[1].startswith('T '):
        raise ValueError('missing "T <time>" header line')
    snap_time = int(lines[1][2:])
    miners = []
    for ln in lines[2:]:
        if not ln:
            continue
        addr, tenure, power = ln.split('\t')
        miners.append({'addr': addr, 'tenure_secs': int(tenure), 'power': int(power)})
    return snap_time, miners

def candidates(miners, min_days, min_power, days_full, power_full):
    """days = tenure/86400; gate on min_days & min_power; weight = (min(days/DF,1)+min(power/PF,1))/2."""
    out = []
    for m in miners:
        days = m['tenure_secs'] / 86400.0
        power = float(m['power'])
        if days < min_days or power < min_power:
            continue
        dn = min(days / days_full, 1.0)
        pn = min(power / power_full, 1.0)
        out.append({'addr': m['addr'], 'weight': (dn + pn) / 2.0})
    return out

def opreturn_hash(coinbase_hex):
    """Find the CHIRP OP_RETURN (0x6a <len> 'CHRP1' <32>) in a coinbase tx and return the 32-byte hash hex."""
    b = bytes.fromhex(coinbase_hex)
    tag = b'CHRP1'
    needle = bytes([0x6a, len(tag) + 32]) + tag
    i = b.find(needle)
    if i < 0:
        return None
    start = i + len(needle)
    return b[start:start + 32].hex()

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('snapshot', help='the published <hash>.chirpsnap file')
    ap.add_argument('--expect-hash', help='commitment hash (hex) to check the snapshot against')
    ap.add_argument('--coinbase-hex', help='raw coinbase tx hex; the CHRP1 OP_RETURN is parsed from it')
    ap.add_argument('--prevhash', help='previous block hash (as shown by getblockhash) — the draw seed')
    ap.add_argument('--total', type=int, help='coinbase value in sats (subsidy + fees) to replay the split')
    ap.add_argument('--fee-bps', type=int, default=90)
    ap.add_argument('--max-n', type=int, default=100)
    ap.add_argument('--min-payout', type=int, default=1000)
    ap.add_argument('--min-days', type=float, default=7.0)
    ap.add_argument('--min-power', type=float, default=5000000.0)
    ap.add_argument('--days-full', type=float, default=30.0)
    ap.add_argument('--power-full', type=float, default=2000000000.0)
    a = ap.parse_args()

    raw = open(a.snapshot, 'rb').read()
    got = hashlib.sha256(raw).hexdigest()
    snap_time, miners = parse_snapshot(raw)
    print(f"snapshot: {len(miners)} miners, T={snap_time}, sha256={got}")

    expect = a.expect_hash
    if not expect and a.coinbase_hex:
        expect = opreturn_hash(a.coinbase_hex)
        if expect is None:
            print("  (no CHRP1 OP_RETURN found in --coinbase-hex)")
    if expect:
        ok = expect.lower() == got.lower()
        print(f"commitment: {'OK ✓' if ok else 'MISMATCH ✗'}  (on-chain {expect.lower()})")
        if not ok:
            sys.exit(1)

    if a.prevhash and a.total is not None:
        seed = int(a.prevhash[-16:], 16)  # first 8 bytes internal LE = last 16 hex of the display hash, big-endian
        cands = candidates(miners, a.min_days, a.min_power, a.days_full, a.power_full)
        payouts, pool_total = split(cands, a.total, a.fee_bps, seed, a.max_n, a.min_payout)
        print(f"replayed draw (seed={seed:#018x}): {len(cands)} candidates, {len(payouts)} payouts, pool_total={pool_total}")
        for p in payouts:
            print(f"  {p['sats']:>14} sat  {p['addr']}")
        print("  → compare these against the block's coinbase outputs.")

if __name__ == '__main__':
    main()
