#!/usr/bin/env python3
"""
Verify a CHIRP payout end-to-end from public data — no trust in the pool required beyond the
published snapshot, which the block itself commits to.

Every CHIRP coinbase carries an OP_RETURN output:  "CHIRP1" || BLAKE2b-256(snapshot)
where `snapshot` is the canonical JSON (sorted keys, compact, integers/strings only) of the exact
registry state the draw used: eligible candidates with tenure (active_secs), 24h work (power), the
weight bits, the parameters, the seed and the resulting payouts.

What this tool checks:
  1. the snapshot's canonical bytes hash to the commitment in the block's coinbase
  2. weight == (min(days/days_full,1) + min(power/power_full,1)) / 2 for every candidate
  3. the Efraimidis–Spirakis draw with seed and the ∝-weight split reproduce the snapshot's payouts
  4. every payout appears in the coinbase with exactly those sats

Usage:
  verify_chirp_block.py --snapshot FILE                          # 2 + 3 on a snapshot file
  verify_chirp_block.py --height H --conf rpc.conf --snapshot-dir DIR   # 1..4 for a mined block
  verify_chirp_block.py --height H --conf rpc.conf --url https://b.pyblock.xyz:8443/chirp_api.php
Exit 0 = everything matches, 1 = mismatch, 2 = usage / missing data.
"""
import argparse, base64, hashlib, json, math, os, struct, sys, urllib.request

TAG = b"CHIRP1"
MASK = (1 << 64) - 1


def canon(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def b2b(b: bytes) -> bytes:
    return hashlib.blake2b(b, digest_size=32).digest()


def u01(seed: int, addr: str) -> float:
    """Port of chirp_u01(): FNV-1a over the address seeded with `seed`, then splitmix64, then /UINT64_MAX."""
    h = 0xcbf29ce484222325 ^ seed
    for byte in addr.encode():
        h ^= byte
        h = (h * 0x00000100000001b3) & MASK
    h ^= h >> 30; h = (h * 0xbf58476d1ce4e5b9) & MASK
    h ^= h >> 27; h = (h * 0x94d049bb133111eb) & MASK
    h ^= h >> 31
    v = float(h) / float(MASK)
    return max(1e-12, min(1.0, v))


def bits_to_double(s: str) -> float:
    return struct.unpack("<d", struct.pack("<Q", int(s, 16)))[0]


def recompute(snap: dict):
    """Returns (problems, payouts) — payouts recomputed from the snapshot's candidates and parameters."""
    problems = []
    days_full, power_full = float(snap["days_full"]), float(snap["power_full"])
    seed = int(snap["seed"])
    cands = []
    for c in snap["candidates"]:
        w_bits = bits_to_double(c["weight_bits"])
        dn = min(c["active_secs"] / 86400.0 / days_full, 1.0)
        pn = min(float(c["power"]) / power_full, 1.0)
        w_re = (dn + pn) / 2.0
        if abs(w_re - w_bits) > 1e-9:
            problems.append(f"weight mismatch for {c['addr']}: snapshot {w_bits!r} vs recomputed {w_re!r}")
        cands.append((c["addr"], w_bits))
    # Efraimidis–Spirakis: key = u^(1/w), top max_n
    keyed = [(math.pow(u01(seed, a), 1.0 / w), a, w) for a, w in cands if w > 0]
    keyed.sort(key=lambda t: -t[0])
    winners = keyed[: int(snap["max_n"])]
    total = int(snap["coinbase_value"]); fee_bps = int(snap["fee_bps"])
    base_fee = (total * fee_bps) // 10000
    distributable = total - base_fee if total > base_fee else 0
    total_w = sum(w for _, _, w in winners)
    payouts = []
    if winners and total_w > 0:
        for _, a, w in winners:
            amt = int(float(distributable) * (w / total_w))
            if amt >= int(snap["min_payout_sats"]):
                payouts.append((a, amt))
    if payouts != [(p["addr"], int(p["sats"])) for p in snap["payouts"]]:
        problems.append("draw/split does not reproduce the snapshot's payouts")
    assigned = sum(s for _, s in payouts)
    if total - assigned != int(snap["pool_total"]):
        problems.append(f"pool_total mismatch: snapshot {snap['pool_total']} vs recomputed {total - assigned}")
    return problems, payouts


def load_snapshot_by_hash(hx: str, snapshot_dir: str | None, url: str | None) -> bytes:
    if snapshot_dir:
        p = os.path.join(snapshot_dir, hx + ".json")
        if os.path.exists(p):
            return open(p, "rb").read()
    if url:
        with urllib.request.urlopen(f"{url}?mode=snapshot&hash={hx}", timeout=10) as r:
            return r.read()
    raise SystemExit(f"snapshot {hx} not found (looked in {snapshot_dir!r} / {url!r})")


def rpc_factory(conf: str):
    c = dict(l.strip().split("=", 1) for l in open(conf) if "=" in l and not l.startswith("#"))
    url = f"http://{c.get('rpcconnect', '127.0.0.1')}:{c['rpcport']}/"
    auth = base64.b64encode(f"{c['rpcuser']}:{c['rpcpassword']}".encode()).decode()

    def rpc(m, *p):
        req = urllib.request.Request(url, json.dumps({"method": m, "params": list(p), "id": 1}).encode(),
                                     {"Authorization": "Basic " + auth, "Content-Type": "application/json"})
        r = json.load(urllib.request.urlopen(req, timeout=60))
        if r.get("error"): raise SystemExit(f"rpc {m}: {r['error']}")
        return r["result"]
    return rpc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--snapshot"); ap.add_argument("--height", type=int); ap.add_argument("--conf")
    ap.add_argument("--snapshot-dir"); ap.add_argument("--url")
    a = ap.parse_args()
    ok = True

    if a.snapshot and not a.height:
        raw = open(a.snapshot, "rb").read(); snap = json.loads(raw)
        print(f"snapshot h={snap['height']} prevhash={snap['prevhash'][:16]}… candidates={len(snap['candidates'])} payouts={len(snap['payouts'])} hash={b2b(canon(snap)).hex()}")
        if canon(snap) != raw.strip():
            print("note: file is not in canonical form (re-serialized for hashing)")
        problems, _ = recompute(snap)
        for p in problems: print("FAIL", p)
        print("OK snapshot reproduces its own draw and split" if not problems else "MISMATCH")
        return 0 if not problems else 1

    if not (a.height and a.conf):
        ap.print_help(); return 2
    rpc = rpc_factory(a.conf)
    bh = rpc("getblockhash", a.height); blk = rpc("getblock", bh, 2); cb = blk["tx"][0]
    commit = None; outs = {}
    for o in cb["vout"]:
        spk = bytes.fromhex(o["scriptPubKey"]["hex"])
        if spk[:2] == bytes([0x6a, 0x26]) and spk[2:8] == TAG and len(spk) == 40:
            commit = spk[8:].hex()
        elif o["value"] > 0 and o["scriptPubKey"].get("address"):
            outs[o["scriptPubKey"]["address"]] = outs.get(o["scriptPubKey"]["address"], 0) + round(o["value"] * 1e8)
    if not commit:
        print(f"block {a.height}: no CHIRP1 commitment in the coinbase (pre-commitment block or not a CHIRP block)"); return 2
    print(f"block {a.height} {bh[:16]}… commitment={commit}")
    raw = load_snapshot_by_hash(commit, a.snapshot_dir, a.url); snap = json.loads(raw)
    h = b2b(canon(snap)).hex()
    if h != commit:
        print(f"FAIL snapshot hash {h} != commitment {commit}"); ok = False
    else:
        print("OK snapshot hashes to the commitment")
    if snap["height"] != a.height or snap["prevhash"] != blk["previousblockhash"]:
        print(f"FAIL snapshot is for h={snap['height']} prev={snap['prevhash'][:16]}…, block has prev={blk['previousblockhash'][:16]}…"); ok = False
    problems, payouts = recompute(snap)
    for p in problems: print("FAIL", p); ok = False
    if not problems: print(f"OK draw and split reproduce {len(payouts)} payouts from the committed registry state")
    missing = [(addr, sats) for addr, sats in payouts if outs.get(addr) != sats]
    if missing:
        for addr, sats in missing: print(f"FAIL coinbase does not pay {addr} exactly {sats} sats (got {outs.get(addr)})"); ok = False
    else:
        print(f"OK every payout is in the coinbase with the exact amount (pool leftover {snap['pool_total']} sats)")
    print("VERIFIED" if ok else "MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
