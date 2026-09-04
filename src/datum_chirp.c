// datum_chirp.c — CHIRP economic engine, port fiel de chirp.rs (whitepaper v1.0).
// Puro (sin deps de datum): registro por-address, elegibilidad+peso, sorteo Efraimidis–Spirakis, split.
#include "datum_chirp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void chirp_init(chirp_registry_t *r){ r->miners=NULL; r->n=0; r->cap=0; }
void chirp_free(chirp_registry_t *r){
    for(size_t i=0;i<r->n;i++) free(r->miners[i].shares);
    free(r->miners); r->miners=NULL; r->n=0; r->cap=0;
}

static chirp_miner_t* find_or_add(chirp_registry_t *r, const char *addr, uint64_t now){
    for(size_t i=0;i<r->n;i++) if(strncmp(r->miners[i].addr,addr,CHIRP_ADDR_MAX-1)==0) return &r->miners[i];
    if(r->n==r->cap){ r->cap = r->cap? r->cap*2 : 16; r->miners = realloc(r->miners, r->cap*sizeof(chirp_miner_t)); }
    chirp_miner_t *m=&r->miners[r->n++];
    memset(m,0,sizeof(*m));
    strncpy(m->addr,addr,CHIRP_ADDR_MAX-1);
    m->first_seen=now; m->last_seen=0; m->active_secs=0.0;
    return m;
}

// Σ work en la ventana 24h (poda entradas viejas in-place). Igual a MinerStats::window_work.
static double window_work(chirp_miner_t *m, uint64_t now){
    uint64_t cutoff = now>CHIRP_WINDOW_SECS ? now-CHIRP_WINDOW_SECS : 0;
    size_t w=0; double sum=0;
    for(size_t i=0;i<m->n_shares;i++){
        if(m->shares[i].ts>=cutoff){ m->shares[w++]=m->shares[i]; sum+=m->shares[i].work; }
    }
    m->n_shares=w;
    return sum;
}

void chirp_record_share(chirp_registry_t *r, const char *addr, double work, uint64_t now){
    chirp_miner_t *m=find_or_add(r,addr,now);
    uint64_t gap = now>m->last_seen ? now-m->last_seen : 0;
    if(m->last_seen!=0 && gap<CHIRP_ACTIVE_GAP_CAP) m->active_secs += (double)gap;  // tenure ACTIVA (gaps offline no cuentan)
    m->last_seen=now;
    if(m->n_shares==m->cap_shares){ m->cap_shares = m->cap_shares? m->cap_shares*2 : 32; m->shares=realloc(m->shares, m->cap_shares*sizeof(chirp_share_t)); }
    m->shares[m->n_shares].ts=now; m->shares[m->n_shares].work=work; m->n_shares++;
}

// Elegibles con weight del WHITEPAPER §4.2: media de tenure y poder normalizados —
//   weight = ( min(days/DAYS_FULL,1) + min(power/POWER_FULL,1) ) / 2
// (El powersplit weight=power del 2026-09-02 VIOLABA el WP publicado → revertido 2026-09-03 por
// orden de Curly: "tenemos que cumplir con el WP". El WP §Parámetros manda CALIBRAR POWER_FULL
// contra el hashrate vivo → override por ENV CHIRP_POWER_FULL / CHIRP_DAYS_FULL sin recompilar.)
static double chirp_env_or(const char *name, double dflt){
    const char *e = getenv(name);
    return (e && *e) ? atof(e) : dflt;
}
size_t chirp_candidates(chirp_registry_t *r, uint64_t now, double min_days, double min_power, chirp_cand_t *out, size_t max_out){
    double days_full  = chirp_env_or("CHIRP_DAYS_FULL",  CHIRP_DAYS_FULL);
    double power_full = chirp_env_or("CHIRP_POWER_FULL", CHIRP_POWER_FULL);
    if(days_full<=0)  days_full  = CHIRP_DAYS_FULL;
    if(power_full<=0) power_full = CHIRP_POWER_FULL;
    size_t k=0;
    for(size_t i=0;i<r->n && k<max_out;i++){
        chirp_miner_t *m=&r->miners[i];
        double days = m->active_secs/86400.0;
        double power = window_work(m, now);
        if(days<min_days || power<min_power) continue;               // quien no cumple, no es aspirante
        double dn = days/days_full;   if(dn>1.0) dn=1.0;
        double pn = power/power_full; if(pn>1.0) pn=1.0;
        strncpy(out[k].addr,m->addr,CHIRP_ADDR_MAX-1); out[k].addr[CHIRP_ADDR_MAX-1]=0;
        out[k].weight=(dn+pn)/2.0;                                   // WP §4.2: la MEDIA equilibra — ni solo lealtad ni solo hashrate
        k++;
    }
    return k;
}

// Uniforme determinista (0,1] por (seed,address). FNV-1a + splitmix64 (idéntico a chirp.rs::u01).
double chirp_u01(uint64_t seed, const char *addr){
    uint64_t h = 0xcbf29ce484222325ULL ^ seed;
    for(const unsigned char *p=(const unsigned char*)addr; *p; p++){
        h ^= (uint64_t)(*p);
        h *= 0x00000100000001b3ULL;
    }
    h ^= h>>30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h>>27; h *= 0x94d049bb133111ebULL;
    h ^= h>>31;
    double v = (double)h / (double)UINT64_MAX;
    if(v<1e-12) v=1e-12; if(v>1.0) v=1.0;
    return v;
}

typedef struct { double key; const chirp_cand_t *c; } keyed_t;
static int keyed_cmp_desc(const void *a, const void *b){
    double ka=((const keyed_t*)a)->key, kb=((const keyed_t*)b)->key;
    if(kb>ka) return 1; if(kb<ka) return -1; return 0;
}

// Muestreo ponderado SIN reemplazo (Efraimidis–Spirakis: key=u^(1/w), top n). Determinista por seed.
size_t chirp_weighted_draw(const chirp_cand_t *cands, size_t nc, uint64_t seed, size_t n, chirp_cand_t *out){
    keyed_t *kd = malloc(nc*sizeof(keyed_t)); if(!kd) return 0;
    size_t m=0;
    for(size_t i=0;i<nc;i++){
        if(cands[i].weight<=0.0) continue;
        double key = pow(chirp_u01(seed,cands[i].addr), 1.0/cands[i].weight);
        kd[m].key=key; kd[m].c=&cands[i]; m++;
    }
    qsort(kd,m,sizeof(keyed_t),keyed_cmp_desc);
    size_t take = m<n? m:n;
    for(size_t i=0;i<take;i++) out[i]=*kd[i].c;
    free(kd);
    return take;
}

// Split CHIRP: fee_bps del total al pool, resto distribuido ∝ weight entre los ganadores del sorteo.
// Devuelve payouts (>= MIN_PAYOUT_SATS) y *pool_total = total - Σasignado (fee + dust caído + redondeo).
size_t chirp_split(const chirp_cand_t *cands, size_t nc, uint64_t total_value, uint16_t fee_bps, uint64_t seed, chirp_payout_t *out, uint64_t *pool_total){
    uint64_t base_fee = (uint64_t)(( (unsigned __int128)total_value * fee_bps) / 10000);
    uint64_t distributable = total_value>base_fee ? total_value-base_fee : 0;
    chirp_cand_t *winners = malloc((nc?nc:1)*sizeof(chirp_cand_t));
    size_t nw = chirp_weighted_draw(cands, nc, seed, CHIRP_MAX_N, winners);
    double total_w=0; for(size_t i=0;i<nw;i++) total_w+=winners[i].weight;
    if(nw==0 || total_w<=0.0){ free(winners); if(pool_total)*pool_total=total_value; return 0; }
    size_t k=0; uint64_t assigned=0;
    for(size_t i=0;i<nw;i++){
        uint64_t amt = (uint64_t)((double)distributable * (winners[i].weight/total_w));
        if(amt>=CHIRP_MIN_PAYOUT_SATS){
            strncpy(out[k].addr,winners[i].addr,CHIRP_ADDR_MAX-1); out[k].addr[CHIRP_ADDR_MAX-1]=0;
            out[k].sats=amt; assigned+=amt; k++;
        }
    }
    if(pool_total)*pool_total = total_value>assigned ? total_value-assigned : 0;
    free(winners);
    return k;
}

// payout_address: sri.lotto.<addr>.<worker> → <addr> ; <addr>.<worker> → <addr>.
void chirp_payout_address(const char *user_identity, char *out, size_t outsz){
    const char *s=user_identity;
    if(strncmp(s,"sri.lotto.",10)==0) s+=10;
    // copiar hasta el primer '.' (separador de worker)
    size_t i=0; for(; s[i] && s[i]!='.' && i<outsz-1; i++) out[i]=s[i];
    out[i]=0;
}
