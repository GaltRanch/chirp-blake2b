# CHIRP × Carousel — the syndicate mines sovereign templates

**Status:** implemented in this tree (unified with the Carousel gateway), proven on regtest and on a mainnet
staging gateway (no miners). Production split decided by PyBLOCK: **98 % syndicate · 1 % template supplier ·
1 % pool** (`chirp_fee_bps: 100`, `template_supplier_bps: 100`).

## Why

CHIRP already answers *who gets paid* (a weighted, verifiable lottery among syndicate members, committed in the
coinbase — see `chirp-snapshot-commitment.md`). Carousel answers *whose block template gets mined* (a deterministic
rotation among Template Suppliers whose templates the node validated — see the `datum-carousel` repo,
`docs/design/carousel-rotation.md`). Running both in one gateway means the syndicate's hashrate builds blocks
selected by the miners' own nodes, and the supplier of the winning template is paid on-chain by the same coinbase
that pays the members. Nothing custodial is added: the coinbase is the settlement.

## One gateway, two modes

The code base is the Carousel gateway (`build-tmpl`) plus the CHIRP engine (`datum_chirp.c`,
`datum_chirp_glue.c`) and three hooks:

| Hook | Where | What |
|---|---|---|
| share record | `datum_stratum.c` `stratum_note_share()` | every accepted share feeds the CHIRP registry (tenure + 24 h work) when `blake2b_chirp` is on |
| coinbase fill | `datum_coinbaser.c`, solo branch | `chirp_glue_fill_outputs()` fills the shared payout outputs; the job's coinbase index becomes 4 (payout split) so the BLAKE2b commitment, the job id and the submit path all point at the same coinbase (PR #17 invariant) |
| config | `datum_conf.c` | `blake2b_chirp` (bool), `chirp_fee_bps` (int, default 90). Clamps: `chirp_fee_bps + template_supplier_bps < 10000`; `blake2b_chirp` and `blake2b_personal_lotto` are mutually exclusive (shared vs per-miner coinbase) |

`blake2b_template` / `blake2b_template_carousel` / `template_dir` / `template_require_validated` are the Carousel
options and work unchanged: the template thread swaps the node's template for the rotation's pick, pins the
supplier on the job (`carousel_supplier`) and tags the scriptsig `<tag>/<supplier name>`.

## The coinbase

```
vout[0]  OP_RETURN "CHIRP1" || BLAKE2b-256(snapshot v2)          0 sat
vout[1]  template supplier      coinbase_value × supplier_bps / 10000   (only when a template was injected)
vout[2…] syndicate winners       ∝ weight, drawn from split_total = coinbase_value − supplier_sats
vout[n]  pool                    leftover = chirp_fee_bps of split_total + dust/rounding
```

The supplier takes its share of the **whole** coinbase (subsidy + the fees of *its* template); the draw splits the
rest exactly as classic CHIRP does. With no fresh validated template for the current prevhash (`failed=true` or an
empty set) the gateway falls back to the node's own template and the coinbase is classic CHIRP: no supplier output.

## Snapshot v2

The committed snapshot gains `v: 2`, `supplier`, `supplier_bps`, `supplier_sats`, `split_total`, and the
**complete registry** (`registry[]`, one entry per member with `active_secs`, `power` and `eligible`) so a verifier
re-checks the gate as well as the draw — the follow-up proposed by Kilombino in PR #1, as is the
`CHIRP_SNAPSHOT_COMMIT=0` kill-switch. `tools/verify_chirp_block.py` verifies v1 and v2 blocks and compares payouts
by scriptPubKey (network-agnostic).

## Evidence

* regtest, unified binary, supplier simulated from the regtest node's own `getblocktemplate` with the ingest's
  `validated` stamp: block 881 — OP_RETURN + supplier 1.000 % + 30 member payouts (30/30 exact) + pool 0.990 %,
  scriptsig `PyBLOCK-CHIRP-BLAKE2b/regtest-supplier`, `verify_chirp_block.py` → `VERIFIED`; block 882 (no fresh
  template) — classic CHIRP 99/1, `VERIFIED`. Both accepted by the node.
* mainnet staging (no miners): rotation set n=28 real suppliers, `CHIRP fill … supplier=<real supplier>
  supplier_sats=1 %` on every cycle, no errors.

## Operating notes

* Members' share goes from 99.1 % to 98 % when the mode is enabled; announce before enabling.
* Suppliers need nothing new: the same template ingest serves Carousel and CHIRP × Carousel.
* Verification path for a block: `verify_chirp_block.py --height H --conf bitcoin.conf --snapshot-dir …` (snapshot
  v2) plus `verify_carousel_rotation.py` from `datum-carousel` for the template pick.
