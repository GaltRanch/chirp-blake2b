// datum_chirp_snapshot.h — CHIRP registry-snapshot commitment.
//
// Commits a hash of the *exact* registry snapshot used for a block's draw into
// the block's own coinbase (an OP_RETURN output), and writes that snapshot to a
// file so the pool can publish it. A verifier fetches the published snapshot,
// re-hashes it (must equal the on-chain commitment) and can recompute the whole
// draw+split from it — so the pool is bound on-chain to the tenure/power figures
// it used, and can no longer rewrite them after the fact. See
// docs/chirp_snapshot_commitment.md.
#ifndef DATUM_CHIRP_SNAPSHOT_H
#define DATUM_CHIRP_SNAPSHOT_H

#include "datum_chirp.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Canonical snapshot tag/version. Bump only on a format change.
#define CHIRP_SNAPSHOT_MAGIC   "CHIRPSNAP1"
// OP_RETURN payload tag (5 bytes) that precedes the 32-byte hash.
#define CHIRP_SNAPSHOT_OPTAG   "CHRP1"

// Serialize the registry into the canonical snapshot bytes (see the .c for the
// exact format). Deterministic: miners sorted by address, integer-only fields.
// On success returns the length and sets *out to a malloc'd buffer (caller frees).
// Returns 0 and leaves *out=NULL on failure.
size_t chirp_snapshot_serialize(const chirp_registry_t *r, uint64_t now, char **out);

// SHA256 of the canonical snapshot bytes. Returns true on success.
bool chirp_snapshot_hash(const chirp_registry_t *r, uint64_t now, unsigned char out32[32]);

// Build the coinbase commitment script: OP_RETURN <push 37> "CHRP1" || hash32.
// Writes into script[cap] and returns its length (39), or 0 if cap is too small.
int chirp_snapshot_opreturn(const unsigned char hash32[32], unsigned char *script, int cap);

// Write the canonical snapshot bytes to <dir>/<hash_hex>.chirpsnap so the pool
// can publish it (the file is named by its own hash). dir is created if missing.
// Returns true on success. Used for transparency; a failure here is non-fatal.
bool chirp_snapshot_publish(const chirp_registry_t *r, uint64_t now, const char *dir);

#endif
