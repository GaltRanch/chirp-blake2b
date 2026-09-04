# CHIRP

### Coinbase Hashrate-Indexed Reward Payouts
**A non-custodial, weighted-lottery shared-reward scheme for Bitcoin mining**

*A PyBLOCK White Paper — v1.0, June 2026*

> *"Sin power no generás la onda gravitacional, y sin tiempo no hay espacio-tiempo."*
> A gravitational wave needs an energetic source **and** spacetime to travel through. CHIRP needs
> the same two things from you: **power** and **time**.

---

## Abstract

CHIRP is a Bitcoin mining-pool reward scheme in which a found block's **coinbase transaction
itself** pays a set of contributing miners directly — atomically, on-chain, and **without the
pool ever holding a single satoshi of anyone's earnings**. Because the coinbase output limit
admits only finitely many recipients, CHIRP fills those slots with a **verifiable weighted
lottery** whose tickets are earned by two things at once: **tenure** (how long you have mined with
PyBLOCK) and **power** (the work you have contributed in a rolling window). Over many blocks, a
miner's expected earnings converge to their weight; in any single block, who gets paid is a
publicly auditable draw seeded by the previous block hash. CHIRP keeps the lottery soul of solo
mining, rewards loyalty and contribution together, and inherits the trust-minimization of paying
miners straight from the block — the Bitcoin network, not the operator, delivers the coins.

---

## 1. The dilemma every small pool faces

A shared mining pool can only ever pay its members **when it finds a block**. The alternative —
FPPS, paying per share regardless of luck — forces the operator to **front every payout from
reserves** and absorb all variance, which is bankruptcy waiting to happen for anyone without
warehouse-scale hashrate.

The honest scheme at small scale is therefore some form of *pay-per-last-N-shares*: rare, large,
fairly-split payouts. But PPLNS as usually deployed has a second problem — **custody**. The block
reward lands in the operator's wallet, and the operator then distributes it. That makes the pool a
custodian of third-party funds, with all the trust, accounting, and regulatory weight that
implies.

CHIRP removes the custody problem entirely.

## 2. Prior art

- **PPS / FPPS** — predictable pay, operator-fronted, variance on the operator. Needs scale and
  reserves. Out of reach (and out of character) for a small cypherpunk pool.
- **PPLNS** — pay over a window of recent shares. Fair, but typically custodial.
- **OCEAN's TIDES** — *Transparent Index of Distinct Extended Shares.* A PPLNS variant over a
  rolling window of the **last 8 blocks** (`8 × network_difficulty` of share-work) that pays each
  miner **directly in the coinbase** — non-custodial and auditable. TIDES is the closest prior art
  and the inspiration for CHIRP. CHIRP differs in two ways: a **bounded window** suited to a small
  pool, and a **weighted-lottery inclusion** that rewards loyalty as a first-class input.

## 3. CHIRP in one sentence

> When the pool solves a block, its coinbase pays a weighted-lottery sample of eligible miners
> directly on-chain, where each miner's odds and slice are the average of how long they've mined
> here (**days**) and how much work they've recently contributed (**power**).

## 4. The mechanism

### 4.1 Eligibility — the candidacy gate

A miner is an **aspirant** only if they clear *both* floors:

```
candidate  ⟺  days_connected ≥ MIN_DAYS   AND   power ≥ MIN_POWER
```

`MIN_POWER` eliminates dust (no sub-economical outputs); `MIN_DAYS` sets a loyalty floor. *Quien no
cumple, no es aspirante* — fail either and you are simply not in the draw. This single rule
defeats the two classic exploits at once: a CPU that idles "connected for years" never clears
`MIN_POWER`, and a whale that parachutes in with huge hashrate never clears `MIN_DAYS`.

### 4.2 Weight — the average of both

Among candidates, each miner's weight is the **mean** of two normalized factors:

```
weight_i = ( days_norm_i + power_norm_i ) / 2

  days_norm_i  = min( days_i  / DAYS_FULL , 1 )
  power_norm_i = min( power_i / POWER_FULL, 1 )
```

The mean — not a product, not a sum — is deliberate. It **balances** the two inputs so that
neither alone dominates: a veteran with a weak machine is dragged down by low `power_norm`; a
freshly-joined giant is dragged down by low `days_norm`. To win, you need **both** loyalty and
real contribution. `power` is the miner's accumulated **share-work** (Σ share difficulty) inside a
rolling window (§4.5).

### 4.3 The lottery — filling the coinbase slots

A coinbase transaction can carry only so many outputs before it competes with fee-paying
transactions for block weight, so CHIRP admits at most `MAX_N` winners per block. When more
candidates exist than slots, CHIRP **draws** them:

```
seed   = previous block hash (low 64 bits)
winners = weighted_draw(candidates, seed, MAX_N)        # P(selected) ∝ weight_i
```

The draw uses the Efraimidis–Spirakis weighted-reservoir method: each candidate is assigned a key
`u_i^(1/weight_i)`, where `u_i ∈ (0,1)` is derived deterministically from `(seed, address)`; the
top-`MAX_N` keys win. The seed is the **previous block hash** — unknown when miners started
contributing (so it cannot be gamed), yet fully reproducible afterward (so anyone can audit the
draw). The result: **the coinbase-size constraint stops being a limitation and becomes the
mechanism** — a fair, recurring lottery for the slots.

### 4.4 Payout — the split among winners

The block reward, less the pool fee, is divided across the winners in proportion to weight:

```
payout_i = ( R − fee ) × weight_i / Σ_{j ∈ winners} weight_j

  R   = block subsidy + transaction fees
  fee = PyBLOCK's cut (a single coinbase output)
```

Every payout is a coinbase `TxOut`. The pool builds this coinbase into the template each miner
mines; if any miner solves the block, the network pays everyone at once. **No balances, no
withdrawals, no custody.**

### 4.5 The window — a bounded variant of TIDES

`power` is measured over a rolling window of recent share-work. TIDES uses an `8 × network_difficulty`
window; at OCEAN's hashrate that spans days, but at a small pool's hashrate it would span
**millions of years** and never slide. CHIRP therefore bounds the window:

```
window = min( 8 × network_difficulty , last 24 hours )
```

This behaves exactly like TIDES once a pool grows to exahash scale, while staying meaningful and
fair for active miners today. Because share-work is `hashrate × time`, the window already rewards
both contribution and recent presence; the explicit `days`/`power` average in §4.2 layers tenure
on top of it.

## 5. The gravitational-wave principle

CHIRP is named for the **chirp** — the rising signal LIGO detects when two black holes spiral
together and merge, radiating ripples through spacetime. The metaphor is exact, and it is also the
design's first principle:

- **Without power, there is no wave.** Gravitational waves are radiated by accelerating mass-energy.
  No energetic source → no signal. No real hashrate → no CHIRP weight.
- **Without time, there is no spacetime.** The wave is a ripple of *space-time*; remove the temporal
  dimension and there is no medium to ripple. No tenure → no CHIRP weight.

The dual requirement is not an arbitrary rule bolted onto a payout formula. It is **cosmological**:
a chirp, by its physics, demands both an energetic source and time. So does CHIRP. Miners *merge*
their hashrate like colliding black holes, and when the block is found, the reward **chirps** out
as a wave that pays every contributor.

(Ocean → TIDES, tides of water. Gravity → CHIRP, ripples of spacetime.)

## 6. Verifiability & transparency

CHIRP is auditable end to end:

- **Weights** are computed from public inputs — each address's first-seen timestamp (tenure) and
  its rolling share-work (power) — and published.
- **The draw** is deterministic given the previous block hash; anyone can replay
  `weighted_draw(candidates, seed, MAX_N)` and confirm they had the correct odds.
- **The split** is visible directly in the coinbase of every block the pool finds; miners decode
  it and verify their `TxOut` is correct, exactly as they can today.

Every share, every weight, every draw, every payout is checkable. Don't trust — verify.

## 7. Anti-gaming

| Attack | Why it fails |
|---|---|
| **Sybil** (split hashrate across new addresses) | each fresh address starts at 0 days → below `MIN_DAYS` → not a candidate |
| **Idle loyalty** (a CPU "connected for years") | never clears `MIN_POWER`; and the average sinks low power anyway |
| **Whale parachute** (huge power, brand new) | never clears `MIN_DAYS`; `days_norm = 0` sinks the average |
| **Window-boundary hopping** | muted by the 24h smoothing and the lottery's randomness |
| **Predicting the draw** | the seed is the *previous block hash* — unknown until it exists |

## 8. Parameters (v1 defaults, tunable)

| Parameter | Default | Role |
|---|---|---|
| `MIN_DAYS` | 7 days | loyalty floor for candidacy |
| `MIN_POWER` | ≈ 10 shares of work at min-difficulty | dust floor for candidacy |
| `DAYS_FULL` | 30 days | tenure at which `days_norm` saturates |
| `POWER_FULL` | calibrated to a healthy ASIC over 24h | work at which `power_norm` saturates |
| `MAX_N` | 100 | coinbase slots = lottery winners |
| Window | min(8×netDiff, 24h) | rolling power window |
| Fee | 0.9% | PyBLOCK's coinbase cut |

## 9. Implementation & deployment

CHIRP runs as a dedicated Stratum V2 pool instance on **port 5554**, alongside (and entirely
independent of) PyBLOCK's solo/lottery stratum on :5555. Both serve PyBLOCK's own **BIP-110**
templates from the same Bitcoin Knots node, so every CHIRP block also signals BIP-110. The reward
engine is config-gated (`payout_scheme = "chirp"`); a per-address stats registry, updated on every
accepted share and snapshotted to disk, supplies the tenure and power that become each miner's
weight.

## 10. Limitations & honesty

- **Variance is real.** A small pool finds blocks rarely; CHIRP smooths *who shares* a block, not
  *how often* blocks arrive. It is a syndicate, not a salary. Miners who want steady, smoothed
  income should mine DATUM → OCEAN, which PyBLOCK also offers.
- **`POWER_FULL` and `MIN_POWER` require calibration** against live hashrate.
- **Non-custodial ≠ unregulated by physics.** The coinbase is consensus-bound; CHIRP cannot pay
  more than a block is worth, and dust-sized stakes are excluded by design.

## 11. Conclusion

CHIRP turns three constraints into features. The *custody problem* becomes **non-custody** — the
network pays miners directly in the coinbase. The *coinbase-size limit* becomes a **fair, verifiable
lottery** for the slots. And the *small-pool variance* becomes the **honest soul of the product** —
a syndicate that, by the same logic as the cosmos, pays those who bring both energy and time.

Mine on PyBLOCK. Merge your hashrate. Hear the chirp. 🌌

---

*PyBLOCK — The CypherPunk's Bitcoin Mining Pool. Powered by Bitcoin Knots · BIP-110 enforced.*
