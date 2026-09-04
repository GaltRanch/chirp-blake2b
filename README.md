# chirp-blake2b

**CHIRP — a non-custodial mining syndicate on Bitcoin-BLAKE2b. When the pool finds a block, the reward is
split on-chain, straight to each member's own address, by a weighted lottery anyone can verify.**

`chirp-blake2b` is a fork of [DATUM Gateway](https://github.com/OCEAN-xyz/datum_gateway) (OCEAN / Jason
Hughes, MIT) ported to **Bitcoin-BLAKE2b** and extended with the CHIRP economic engine (`src/datum_chirp.c`,
`src/datum_chirp_glue.c`):

- **Per-address ledger** — the gateway records each miner's *active* tenure (gaps offline don't count) and
  24h work window from their shares. Persisted to a JSON registry and reloaded on restart.
- **Eligibility gate** — `CHIRP_MIN_DAYS` of active tenure **and** `CHIRP_MIN_POWER` of recent work.
  Newcomers pay into the pot until they qualify.
- **Weight (whitepaper §4.2)** — `weight = (min(days/DAYS_FULL,1) + min(power/POWER_FULL,1)) / 2`, the mean
  of normalized loyalty and hashrate. `DAYS_FULL` / `POWER_FULL` are calibrated against live hashrate via
  env (`CHIRP_DAYS_FULL`, `CHIRP_POWER_FULL`) without recompiling.
- **Deterministic weighted draw** — Efraimidis–Spirakis sampling without replacement, **seeded by the
  previous block hash**, up to `CHIRP_MAX_N` winners. Anyone can recompute the draw from the chain.
- **Coinbase split** — winners are paid ∝ weight as outputs of the found block's coinbase. Pool fee
  `CHIRP_FEE_BPS` (90 = 0.9%) and sub-dust remainders go to the pool address. Nothing is custodied.

> Status: **public / pre-release.** Running in production on the PyBLØCK BLAKE2b pool (first CHIRP blocks
> paid dozens of miners on-chain in a single coinbase). Not audited.

## Build

Same toolchain as upstream DATUM Gateway (C, CMake). See `docs/UPSTREAM-README.md` for dependencies.

```
mkdir build && cd build
cmake ..
make datum_gateway
```

## Run

```
CHIRP_MIN_DAYS=7 CHIRP_MIN_POWER=1 ./build/datum_gateway -c configs/chirp.example.json
```

Copy the example config, fill in your node RPC credentials and pool address, and **never commit a real
config** (`.gitignore` blocks `configs/*.json` except `*.example.json`).

Runtime knobs (environment, read at start):

| variable | meaning |
|---|---|
| `CHIRP_MIN_DAYS` | active tenure required to be eligible (days) |
| `CHIRP_MIN_POWER` | minimum 24h work to be eligible |
| `CHIRP_DAYS_FULL` | tenure that saturates the loyalty term (default 30) |
| `CHIRP_POWER_FULL` | work that saturates the hashrate term — calibrate to live hashrate |

## Verify a payout yourself

Take the block's previous-block hash as the seed, the registry snapshot at that height, apply the gate,
compute weights, run the draw, split the coinbase value ∝ weight — and compare with the coinbase outputs
on-chain. The whitepaper in `docs/` walks through it.

## Repository layout

```
src/        gateway sources (upstream + BLAKE2b + CHIRP engine)
configs/    *.example.json only — sanitized
docs/       CHIRP whitepaper (md / tex / pdf), payout design, UPSTREAM-README.md
```

## License

- Original DATUM Gateway code: **MIT**, © 2024–2025 Bitcoin Ocean, LLC, Jason Hughes and contributors —
  preserved verbatim in `LICENSE.MIT`.
- BLAKE2b port, the CHIRP engine and all other PyBLØCK additions: **Apache License 2.0** (`LICENSE`),
  © 2026 PyBLØCK. See `NOTICE`.
