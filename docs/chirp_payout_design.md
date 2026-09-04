# CHIRP — Coinbase Hashrate-Indexed Reward Payouts

**Status:** DESIGN LOCKED (2026-06-27). Implementation next.
PyBLOCK's non-custodial shared-reward payout scheme for the SV2 BIP-110 stratum (:5555).
A gravitational-wave-themed variant of OCEAN's TIDES.

---

## 1. What it is
When the pool solves a block, the **coinbase itself** pays a set of contributing miners
directly — atomic, on-chain, **non-custodial** (PyBLOCK never holds anyone's funds; the
custody/compliance blocker of the old custodial-PPLNS design is gone). A weighted **lottery**
picks who fills the limited coinbase output slots; loyalty (days) + real power both raise your
odds. Over many blocks, expected earnings ∝ your weight. Fully auditable.

- **Name:** CHIRP = **C**oinbase **H**ashrate-**I**ndexed **R**eward **P**ayouts. Themed on the
  "chirp" LIGO detects when black holes merge — miners merge hashrate, the reward chirps out as
  a wave to all contributors. (Ocean→TIDES, gravity→CHIRP.)
- **Tagline / first principle:** *"Sin power no generás la onda gravitacional, y sin tiempo no
  hay espacio-tiempo."* The physics itself demands BOTH — a gravitational wave needs an
  energetic source (power) AND spacetime to propagate through (time). So CHIRP requires both:
  power **and** tenure. The dual requirement isn't arbitrary, it's cosmological.

## 2. The mechanism

```
STEP 1 — ELIGIBILITY (candidacy gate)
   Candidate  ⟺  days_connected ≥ MIN_DAYS   AND   power ≥ MIN_POWER
   Whoever fails EITHER minimum is NOT an aspirant — excluded from the draw.
   (MIN_POWER kills dust outputs; MIN_DAYS sets the loyalty floor.)

STEP 2 — WEIGHT (the average — not a product)
   weight_i = (days_norm_i + power_norm_i) / 2
     days_norm_i  = min(days_i / DAYS_FULL, 1)        # normalized tenure 0..1
     power_norm_i = min(power_i / POWER_FULL, 1)      # normalized work  0..1
   The MEAN balances the two: neither factor alone wins. Years on a CPU (low power) →
   low average. A whale that just joined (0 days) → low average. You need BOTH.

STEP 3 — LOTTERY (fill the N coinbase slots)
   Draw up to MAX_N candidates, P(selected) ∝ weight_i (weighted, no replacement).
   Seed = previous block hash → unpredictable beforehand, deterministic & VERIFIABLE after.
   Recomputed each template, so over many blocks expected inclusion ∝ weight.
   The coinbase output limit IS the lottery — the constraint becomes the feature.

STEP 4 — PAYOUT (split among the selected)
   payout_i = (R − fee) × weight_i / Σ_selected weight_j
   R   = block subsidy + tx fees
   fee = pool fee (PyBLOCK's cut)
   All payouts are coinbase TxOuts + one fee TxOut. Non-custodial, atomic on-chain.
```

## 3. "power" = the rolling work window (the TIDES variant)
`power_i` = miner i's accumulated **share-work** (Σ share difficulty) inside a rolling window.

- TIDES uses an 8-block window = `8 × network_difficulty` of work. At OCEAN's hashrate that's
  ~days; at PyBLOCK's ~2 TH/s it would be **millions of years** → never slides → useless.
- **CHIRP window = `min(8 × network_difficulty, last 24h)`** — behaves like TIDES once we scale
  to EH/s, stays bounded & fair for active miners while we're small.
- Share-work already encodes power × time (work = hashrate × duration), so the window
  inherently rewards both contribution and recent presence.

## 4. Anti-gaming
- **Sybil:** splitting hashrate across fresh addresses doesn't help — each new address starts at
  0 days (days_norm = 0) and below MIN_DAYS (not even a candidate).
- **Loyal-but-powerless (CPU for years):** fails MIN_POWER → not a candidate; and the average
  drags low power down anyway.
- **Whale parachute (huge power, brand new):** fails MIN_DAYS; days_norm = 0 sinks the average.
- **Pool-hopping the window boundary:** muted by the 24h smoothing + the lottery randomness.

## 5. Transparency (OCEAN-style, cheap to match)
Publish per-address weights (days + power), the draw seed (prev block hash), MAX_N, and each
block's resulting split. Anyone can replay the weighted draw and verify they had the correct
odds and the coinbase paid the right amounts. Every share, draw, and payout auditable.

## 6. Parameters (defaults — tunable)
| Param | Default | Meaning |
|---|---|---|
| MIN_DAYS | 7 days | loyalty floor to be a candidate |
| MIN_POWER | ≥ ~1000-sat payout equivalent | dust floor to be a candidate |
| DAYS_FULL | 30 days | tenure at which days_norm saturates to 1 |
| POWER_FULL | (calibrate) | work at which power_norm saturates to 1 |
| MAX_N | (pending sidecar coinbase-size limit) | coinbase output slots = lottery winners |
| Window | min(8×netDiff, 24h) | rolling work window for "power" |
| Fee | 0.9% | PyBLOCK's cut (coinbase TxOut) |

## 7. Implementation components
1. **Stats registry (new, persistent):** per payout address → first-seen (→ days) + rolling
   share-work (→ power). Updated on each accepted share. Source of the weights.
   - HOOK POINTS (found): the `ShareValidationResult::Valid` arms in
     `mining_message_handler.rs` — `handle_submit_shares_standard` (~L683) and
     `handle_submit_shares_extended` (~L944). Per accepted share: address = from the channel's
     user_identity/payout_mode; work = `channel.get_target().difficulty_float()` (≈500001 with
     the MIN_DIFFICULTY floor). Add work to that address's rolling window + stamp first-seen.
   - In-memory map for fast access by the coinbase builder; persist to SQLite periodically
     (same WAL pattern as pool_history.db). The 24h window ages out old work.
2. **CHIRP coinbase builder:** in the pool's NewTemplate handler (`handle_new_template`),
   replace the per-channel payout-mode coinbase with ONE pool-wide CHIRP coinbase used for all
   channels: filter candidates → compute weights → seeded weighted draw of MAX_N → build
   TxOuts (winners by weight + fee). Bounded by the SV2 `coinbase_output_max_additional_size`.
3. **Verifiable draw:** deterministic weighted sampling seeded by the previous block hash.
4. **Toggle:** `payout_scheme = "solo" | "chirp"` in pool config (start behind a flag).
5. **Transparency page:** weights, seed, recent splits (site card + public ledger).

## 8. Open / to confirm
- MAX_N from the sidecar's coinbase-output budget.
- POWER_FULL & MIN_POWER calibration (sats/work).
- DECIDED: CHIRP runs as a **separate pool_sv2 instance on port 5554** (the freed translator
  port), alongside the untouched :5555 solo/lotto. Config-gated via `payout_scheme = "chirp"`.
  Same Knots node + GBT template provider (BIP-110) + pool fee address. Zero risk to :5555.
- Legal posture: non-custodial coinbase = miners paid by the network directly (much lighter
  than custodial PPLNS), but confirm before mainnet.
