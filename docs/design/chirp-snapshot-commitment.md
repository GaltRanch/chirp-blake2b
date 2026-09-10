# CHIRP snapshot commitment — making the registry verifiable

> **Credit.** Committing the registry snapshot in the coinbase was proposed by
> [Kilombino](https://github.com/Kilombino) in his review of these repos (2026-09-05), who also
> submitted an independent implementation (#1). The follow-ups — committing the whole registry with an
> `eligible` flag and the `CHIRP_SNAPSHOT_COMMIT` switch — come from that PR.

**Problem.** The CHIRP draw and split are recomputable from the chain, but their *inputs* — each
address's active tenure and 24h work — live in the pool's registry, off-chain. "Anyone can verify"
was therefore conditional: you could check the math, not that the pool didn't inflate a friend's
tenure or work after the fact.

**Mechanism.** Every CHIRP coinbase now carries a zero-value output

```
OP_RETURN  "CHIRP1" || BLAKE2b-256( snapshot )          (6a 26 <38 bytes>, always output #0)
```

where `snapshot` is the exact registry state the draw for that job was computed from, serialized as
canonical JSON — sorted keys, compact separators, **integers and strings only** (no floats), so that
`json.dumps(obj, sort_keys=True, separators=(",", ":"))` in Python yields the same bytes jansson
produced:

```
v, tag="CHIRP1", height, prevhash (display order), seed (decimal string), ts,
coinbase_value, fee_bps, max_n, min_payout_sats,
min_days, min_power, days_full, power_full          (strings, %.17g)
candidates[] = { addr, active_secs (int), power (int, Σ share difficulty in the 24h window),
                 weight_bits (IEEE-754 hex of the double the gateway used) }
payouts[]    = { addr, sats }
pool_total
```

**v2** (CHIRP × Carousel, `v: 2`) adds:

```
supplier, supplier_bps, supplier_sats      template supplier of the job and its share of the WHOLE coinbase
                                           ("" / 0 / 0 when no template was injected)
split_total = coinbase_value − supplier_sats   what the draw actually splits (fee + winners)
registry[]  = { addr, active_secs (int), power (int), eligible (bool) }
                                           the COMPLETE registry the gate ran over — a verifier re-checks
                                           min_days/min_power and that candidates == eligible members
```

`CHIRP_SNAPSHOT_COMMIT=0` disables the commitment (no OP_RETURN, classic coinbase) without a rebuild.
Both follow-ups come from Kilombino's PR #1. See `chirp-carousel.md` for the unified gateway.

The gateway writes the snapshot to `CHIRP_SNAPSHOT_DIR` (`<hash>.json`, 48h buffer) and the pool
publishes it at `chirp_api.php?mode=snapshot&hash=<hash>`; snapshots referenced by found blocks are
archived permanently.

**What becomes verifiable.** With the block and the snapshot, `tools/verify_chirp_block.py` checks:

1. `BLAKE2b-256(snapshot) == commitment` — the published state is the one the block was built on;
   the pool cannot rewrite tenure or work after the block exists.
2. `weight == (min(days/days_full,1) + min(power/power_full,1)) / 2` for every candidate — the
   whitepaper formula was applied.
3. The Efraimidis–Spirakis draw with `seed` and the ∝-weight split reproduce `payouts[]` exactly
   (`u01` = FNV-1a ⊕ seed → splitmix64 → /UINT64_MAX; key = u^(1/w); top `max_n`).
4. Every payout is in the coinbase with exactly those sats.

**What stays trusted.** The registry is still built from shares the pool received: a miner can check
that *their own* `active_secs` and `power` in the snapshot match what they submitted, and anyone can
check that no address's numbers change between consecutive snapshots in ways that shares cannot
explain. Committing the state per block turns "trust the pool" into "catch the pool" — the remaining
step towards zero trust is signed share receipts, which is the same frontier as DATUM-TS.

**Seed.** `seed` = first 8 bytes of the previous block hash in internal (header) byte order, read
little-endian = `int(prevhash_hex[-16:], 16)` of the hash as displayed by `getblockhash`.
