// datum_chirp_snapshot.c — CHIRP registry-snapshot commitment. See the header and
// docs/chirp_snapshot_commitment.md for the rationale and the exact wire format.
#include "datum_chirp_snapshot.h"

#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

// Σ share work inside the 24h window at `now`, WITHOUT mutating the miner (unlike
// window_work() in datum_chirp.c, which prunes in place). Must give the same total
// the draw used, so the commitment matches the state that produced the payouts.
static double snapshot_window_work(const chirp_miner_t *m, uint64_t now) {
	uint64_t cutoff = now > CHIRP_WINDOW_SECS ? now - CHIRP_WINDOW_SECS : 0;
	double sum = 0;
	for (size_t i = 0; i < m->n_shares; i++) {
		if (m->shares[i].ts >= cutoff) sum += m->shares[i].work;
	}
	return sum;
}

static int cmp_miner_ptr(const void *a, const void *b) {
	const chirp_miner_t *ma = *(const chirp_miner_t * const *)a;
	const chirp_miner_t *mb = *(const chirp_miner_t * const *)b;
	return strcmp(ma->addr, mb->addr); // bytewise ascending — same order the verifier sorts
}

// Canonical snapshot bytes (v1), exactly:
//   "CHIRPSNAP1\n"
//   "T <now>\n"
//   for each miner, addresses sorted bytewise ascending:
//     "<addr>\t<active_secs_int>\t<window_work_int>\n"
// active_secs_int = floor(active_secs); window_work_int = llround(Σ window work).
// Both are integer-valued in practice (tenure = Σ integer-second gaps; work = Σ integer
// share difficulties), so this is reproducible bit-for-bit on any machine.
size_t chirp_snapshot_serialize(const chirp_registry_t *r, uint64_t now, char **out) {
	if (!out) return 0;
	*out = NULL;
	if (!r) return 0;

	const chirp_miner_t **idx = NULL;
	if (r->n) {
		idx = malloc(r->n * sizeof(*idx));
		if (!idx) return 0;
		for (size_t i = 0; i < r->n; i++) idx[i] = &r->miners[i];
		qsort(idx, r->n, sizeof(*idx), cmp_miner_ptr);
	}

	size_t cap = 256 + r->n * (CHIRP_ADDR_MAX + 48);
	char *buf = malloc(cap);
	if (!buf) { free(idx); return 0; }
	size_t len = 0;

	int w = snprintf(buf, cap, "%s\nT %llu\n", CHIRP_SNAPSHOT_MAGIC, (unsigned long long)now);
	if (w < 0) { free(buf); free(idx); return 0; }
	len = (size_t)w;

	for (size_t i = 0; i < r->n; i++) {
		const chirp_miner_t *m = idx[i];
		uint64_t tenure = (uint64_t)m->active_secs;               // floor
		double work = snapshot_window_work(m, now);
		uint64_t power = (uint64_t)llround(work);
		// grow if needed
		if (len + CHIRP_ADDR_MAX + 48 >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { free(buf); free(idx); return 0; }
			buf = nb;
		}
		w = snprintf(buf + len, cap - len, "%s\t%llu\t%llu\n",
			m->addr, (unsigned long long)tenure, (unsigned long long)power);
		if (w < 0) { free(buf); free(idx); return 0; }
		len += (size_t)w;
	}

	free(idx);
	*out = buf;
	return len;
}

bool chirp_snapshot_hash(const chirp_registry_t *r, uint64_t now, unsigned char out32[32]) {
	if (!out32) return false;
	char *buf = NULL;
	size_t len = chirp_snapshot_serialize(r, now, &buf);
	if (!buf) return false;
	crypto_hash_sha256(out32, (const unsigned char *)buf, len);
	free(buf);
	return true;
}

int chirp_snapshot_opreturn(const unsigned char hash32[32], unsigned char *script, int cap) {
	const char *tag = CHIRP_SNAPSHOT_OPTAG;         // "CHRP1", 5 bytes
	int taglen = (int)strlen(tag);
	int payload = taglen + 32;                       // 37
	int total = 2 + payload;                         // OP_RETURN + push-len + payload = 39
	if (!hash32 || !script || cap < total || payload > 75) return 0; // 75 = max single direct push
	int o = 0;
	script[o++] = 0x6a;                              // OP_RETURN
	script[o++] = (unsigned char)payload;            // direct push of `payload` bytes
	memcpy(script + o, tag, taglen); o += taglen;
	memcpy(script + o, hash32, 32);  o += 32;
	return o;
}

static void hex32(const unsigned char h[32], char out[65]) {
	static const char *d = "0123456789abcdef";
	for (int i = 0; i < 32; i++) { out[i*2] = d[h[i] >> 4]; out[i*2+1] = d[h[i] & 0xf]; }
	out[64] = 0;
}

bool chirp_snapshot_publish(const chirp_registry_t *r, uint64_t now, const char *dir) {
	if (!dir || !*dir) return false;
	char *buf = NULL;
	size_t len = chirp_snapshot_serialize(r, now, &buf);
	if (!buf) return false;

	unsigned char h[32];
	crypto_hash_sha256(h, (const unsigned char *)buf, len);
	char hx[65]; hex32(h, hx);

	mkdir(dir, 0755); // best effort; ignore EEXIST

	char path[1024], tmp[1024];
	snprintf(path, sizeof(path), "%s/%s.chirpsnap", dir, hx);
	snprintf(tmp,  sizeof(tmp),  "%s/%s.chirpsnap.tmp", dir, hx);
	bool ok = false;
	FILE *f = fopen(tmp, "wb");
	if (f) {
		if (fwrite(buf, 1, len, f) == len) ok = true;
		fclose(f);
		if (ok) rename(tmp, path); else remove(tmp);
	}
	free(buf);
	return ok;
}
