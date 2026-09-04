// datum_chirp.h — CHIRP economic engine (port fiel de chirp.rs / whitepaper v1.0) para chirp_gateway_blake.
// Motor puro: registro por-address (tenure+power), elegibilidad+peso, sorteo verificable, split de coinbase.
#ifndef DATUM_CHIRP_H
#define DATUM_CHIRP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CHIRP_WINDOW_SECS      (24u*3600u)
#define CHIRP_MIN_DAYS         7.0
#define CHIRP_MIN_POWER        5000000.0
#define CHIRP_DAYS_FULL        30.0
#define CHIRP_POWER_FULL       2000000000.0
#define CHIRP_MAX_N            100
#define CHIRP_FEE_BPS          90
#define CHIRP_MIN_PAYOUT_SATS  1000ULL
#define CHIRP_ACTIVE_GAP_CAP   3600u
#define CHIRP_ADDR_MAX         128

typedef struct { uint64_t ts; double work; } chirp_share_t;
typedef struct {
    char addr[CHIRP_ADDR_MAX];
    uint64_t first_seen, last_seen;
    double active_secs;
    chirp_share_t *shares; size_t n_shares, cap_shares;
} chirp_miner_t;
typedef struct { chirp_miner_t *miners; size_t n, cap; } chirp_registry_t;
typedef struct { char addr[CHIRP_ADDR_MAX]; double weight; } chirp_cand_t;
typedef struct { char addr[CHIRP_ADDR_MAX]; uint64_t sats; } chirp_payout_t;

void   chirp_init(chirp_registry_t *r);
void   chirp_free(chirp_registry_t *r);
void   chirp_record_share(chirp_registry_t *r, const char *addr, double work, uint64_t now);
size_t chirp_candidates(chirp_registry_t *r, uint64_t now, double min_days, double min_power, chirp_cand_t *out, size_t max_out);
size_t chirp_weighted_draw(const chirp_cand_t *cands, size_t nc, uint64_t seed, size_t n, chirp_cand_t *out);
size_t chirp_split(const chirp_cand_t *cands, size_t nc, uint64_t total_value, uint16_t fee_bps, uint64_t seed, chirp_payout_t *out, uint64_t *pool_total);
double chirp_u01(uint64_t seed, const char *addr);
void   chirp_payout_address(const char *user_identity, char *out, size_t outsz);
#endif
