# CHIRP — registry-snapshot commitment (phase 2)

**Status:** proposal + reference implementation. Not yet run in production.

## Why

Today the CHIRP draw is *reproducible* — anyone can replay the Efraimidis–Spirakis
draw and the coinbase split from a registry snapshot and the previous block hash.
But the registry itself (each address's **active tenure** and **24 h work**) is
off-chain state that the pool maintains. As the README says honestly, "what
verifiable means here" still trusts the pool not to inflate a favoured address's
days/power. That is the one remaining trust residue.

This phase removes the *retroactive* half of that trust: the pool commits, **in the
found block's own coinbase**, to a hash of the exact registry snapshot it used. Once
the block is mined, the pool is bound to those figures forever and can no longer
rewrite history. Combined with the on-chain seed (prev block hash) and the published
draw, the whole payout becomes tamper-evident.

## What is committed

An `OP_RETURN` output in the coinbase:

```
0x6a  0x25  "CHRP1"  <32-byte SHA256 of the canonical snapshot>
```

`0x6a` = OP_RETURN, `0x25` = 37 = push length, `"CHRP1"` = 5-byte tag, then the hash.
Total script 39 bytes (well under the 80-byte OP_RETURN limit), value 0 sat. Coinbase
policy is set by the miner, so a second OP_RETURN alongside the segwit commitment is
fine; consensus places no limit.

## Canonical snapshot format (v1)

Plain bytes, hashed with SHA256:

```
CHIRPSNAP1\n
T <snapshot_unix_seconds>\n
<addr>\t<active_secs_int>\t<window_work_int>\n      (one line per miner)
```

- Miners are sorted by address, **bytewise ascending** (C `strcmp` == Python `sorted`).
- `active_secs_int` = `floor(active_secs)` — whole seconds of *active* tenure (offline
  gaps already excluded by the engine).
- `window_work_int` = `round(Σ share-work in the last 24 h at T)`.
- Both are integer-valued in practice (tenure is a sum of integer-second gaps; work is
  a sum of integer share difficulties), so the snapshot is reproducible **bit-for-bit**
  on any machine — no floating-point ambiguity in what gets hashed.

`T` (the snapshot time) is included because the 24 h window and the tenure are measured
at that instant; the verifier needs it, and committing it binds the pool to a specific
`(time, state)` pair it cannot later move.

## Publishing

Each time the coinbase is built, the gateway also writes the canonical snapshot to
`<CHIRP_SNAPSHOT_DIR>/<hash_hex>.chirpsnap` (default `./chirp_snapshots`, atomic write).
The file is **named by its own hash**, so publishing that directory is enough: a block's
`OP_RETURN` hash is the filename of the snapshot it committed to.

> Operational note for integration: templates refresh many times per block, so the
> directory accumulates one snapshot per distinct registry state. What must survive for
> a *found* block is the snapshot whose hash equals that block's coinbase OP_RETURN —
> keep the directory (or archive found-block snapshots) rather than pruning aggressively.

## Verifying (anyone)

`tools/verify_chirp_snapshot.py`:

1. hashes the published `<hash>.chirpsnap` and checks it equals the coinbase commitment
   (`--expect-hash`, or parsed from `--coinbase-hex`);
2. with `--prevhash` (the seed) and `--total` (coinbase value), replays the candidate
   gate, the weighted draw and the split, and prints the payouts to compare against the
   block's actual coinbase outputs.

```
tools/verify_chirp_snapshot.py 1e33…a5f0.chirpsnap \
    --coinbase-hex <coinbase raw hex> \
    --prevhash <prev block hash> --total 312500000
```

The draw math is a line-for-line port of `src/datum_chirp.c` (u01 = FNV-1a→splitmix64,
key = `u^(1/weight)`, top-N), and the seed derivation is the one the README documents:
`int(prevhash_hex[-16:], 16)`.

## Config

| var | default | meaning |
|---|---|---|
| `CHIRP_SNAPSHOT_COMMIT` | on | set `0`/`n` to skip the commitment (e.g. testing) |
| `CHIRP_SNAPSHOT_DIR` | `chirp_snapshots` | where the `<hash>.chirpsnap` files are written |

## What this still does *not* cover (honest scope)

The commitment stops the pool from **rewriting** tenure/power after a block is found.
It does not, by itself, stop a pool from **inflating a favoured address in real time
before** committing. Closing that fully is the next step — signed share logs the miners
themselves can attest to, and/or DATUM-TS. This phase is the jump from "trust the
published registry" to "the pool is bound on-chain to whatever it published each block."
