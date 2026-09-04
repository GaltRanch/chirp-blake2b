// datum_chirp_glue.c — CHIRP ↔ datum: registro global thread-safe, persistencia (jansson),
// y relleno del coinbase compartido con el split ponderado (seed = prevhash).
#include "datum_chirp.h"
#include "datum_chirp_glue.h"
#include "datum_stratum.h"
#include "datum_utils.h"
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <jansson.h>
#include "datum_logger.h"

static chirp_registry_t g_chirp;
static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static int      g_inited = 0;
static uint64_t g_last_save = 0;

static const char *chirp_db_path(void){
    const char *e = getenv("CHIRP_DB_PATH");
    return (e && *e) ? e : "/home/curly/datum-blake/chirp_stats_blake.json";
}

// ── persistencia (jansson): {"miners":{addr:{first_seen,last_seen,active_secs,shares:[[ts,work],...]}}} ──
static void chirp_load_locked(uint64_t now){
    json_error_t err; json_t *root = json_load_file(chirp_db_path(), 0, &err);
    if(!root) return;
    json_t *miners = json_object_get(root, "miners");
    if(json_is_object(miners)){
        const char *addr; json_t *mo;
        uint64_t cutoff = now > CHIRP_WINDOW_SECS ? now - CHIRP_WINDOW_SECS : 0;
        json_object_foreach(miners, addr, mo){
            // sembrar el miner
            chirp_record_share(&g_chirp, addr, 0.0, now);   // crea/toca; corregimos abajo
            for(size_t i=0;i<g_chirp.n;i++){
                if(strncmp(g_chirp.miners[i].addr,addr,CHIRP_ADDR_MAX-1)) continue;
                chirp_miner_t *m=&g_chirp.miners[i];
                m->n_shares=0;   // limpiar el share dummy que metió record_share
                m->first_seen = (uint64_t)json_integer_value(json_object_get(mo,"first_seen"));
                m->last_seen  = (uint64_t)json_integer_value(json_object_get(mo,"last_seen"));
                m->active_secs= json_number_value(json_object_get(mo,"active_secs"));
                if(m->first_seen==0) m->first_seen=now;
                json_t *sh=json_object_get(mo,"shares");
                if(json_is_array(sh)){
                    size_t k; json_t *pair;
                    json_array_foreach(sh, k, pair){
                        if(!json_is_array(pair)||json_array_size(pair)<2) continue;
                        uint64_t ts=(uint64_t)json_integer_value(json_array_get(pair,0));
                        double wk=json_number_value(json_array_get(pair,1));
                        if(ts<cutoff) continue;
                        if(m->n_shares==m->cap_shares){ m->cap_shares=m->cap_shares?m->cap_shares*2:32; m->shares=realloc(m->shares,m->cap_shares*sizeof(chirp_share_t)); }
                        m->shares[m->n_shares].ts=ts; m->shares[m->n_shares].work=wk; m->n_shares++;
                    }
                }
                break;
            }
        }
    }
    json_decref(root);
}

static void chirp_save_locked(void){
    json_t *root=json_object(), *miners=json_object();
    for(size_t i=0;i<g_chirp.n;i++){
        chirp_miner_t *m=&g_chirp.miners[i];
        json_t *mo=json_object();
        json_object_set_new(mo,"first_seen", json_integer((json_int_t)m->first_seen));
        json_object_set_new(mo,"last_seen",  json_integer((json_int_t)m->last_seen));
        json_object_set_new(mo,"active_secs",json_real(m->active_secs));
        json_t *sh=json_array();
        for(size_t k=0;k<m->n_shares;k++){
            json_t *pair=json_array(); json_array_append_new(pair,json_integer((json_int_t)m->shares[k].ts)); json_array_append_new(pair,json_real(m->shares[k].work));
            json_array_append_new(sh,pair);
        }
        json_object_set_new(mo,"shares",sh);
        json_object_set_new(miners,m->addr,mo);
    }
    json_object_set_new(root,"miners",miners);
    char tmp[512]; snprintf(tmp,sizeof(tmp),"%s.tmp",chirp_db_path());
    if(json_dump_file(root,tmp,JSON_COMPACT)==0) rename(tmp,chirp_db_path());
    json_decref(root);
}

void chirp_glue_init(void){
    pthread_mutex_lock(&g_lock);
    if(!g_inited){ chirp_init(&g_chirp); chirp_load_locked((uint64_t)time(NULL)); g_inited=1; g_last_save=(uint64_t)time(NULL); }
    pthread_mutex_unlock(&g_lock);
}

void chirp_glue_record(const char *username, uint64_t diff){
    if(!username||!*username) return;
    char addr[CHIRP_ADDR_MAX]; chirp_payout_address(username, addr, sizeof(addr));
    if(!*addr) return;
    uint64_t now=(uint64_t)time(NULL);
    pthread_mutex_lock(&g_lock);
    if(!g_inited){ chirp_init(&g_chirp); chirp_load_locked(now); g_inited=1; g_last_save=now; }
    chirp_record_share(&g_chirp, addr, (double)diff, now);
    pthread_mutex_unlock(&g_lock);
}

void chirp_glue_maybe_save(void){
    uint64_t now=(uint64_t)time(NULL);
    pthread_mutex_lock(&g_lock);
    if(g_inited && now - g_last_save >= 30){ chirp_save_locked(); g_last_save=now; }
    pthread_mutex_unlock(&g_lock);
}

// Rellena job->available_coinbase_outputs[] con [ganador_i ∝ weight …]. El leftover (fee 0.9% + dust)
// lo paga datum al pool_addr automáticamente. seed = primeros 8 bytes de prevhash_bin (LE). Devuelve #outputs.
int chirp_glue_fill_outputs(void *job){
    T_DATUM_STRATUM_JOB *s = (T_DATUM_STRATUM_JOB*)job;
    if(!s || s->coinbase_value==0) return 0;
    uint64_t now=(uint64_t)time(NULL);
    uint64_t seed=0; for(int i=0;i<8;i++) seed |= ((uint64_t)s->prevhash_bin[i])<<(8*i);
    int written=0;
    pthread_mutex_lock(&g_lock);
    if(!g_inited){ chirp_init(&g_chirp); chirp_load_locked(now); g_inited=1; g_last_save=now; }
    size_t cap=g_chirp.n?g_chirp.n:1;
    chirp_cand_t *cands=malloc(cap*sizeof(chirp_cand_t));
    chirp_payout_t payouts[CHIRP_MAX_N]; uint64_t pool_total=0;
    // umbrales del sindicato: ENV override (para tunear/bootstrap sin recompilar) → fallback a los #define.
    double min_days = CHIRP_MIN_DAYS, min_power = CHIRP_MIN_POWER;
    { const char *e;
      if((e=getenv("CHIRP_MIN_DAYS"))  && *e) min_days  = atof(e);
      if((e=getenv("CHIRP_MIN_POWER")) && *e) min_power = atof(e); }
    size_t nc=chirp_candidates(&g_chirp, now, min_days, min_power, cands, cap);
    size_t np=chirp_split(cands, nc, s->coinbase_value, CHIRP_FEE_BPS, seed, payouts, &pool_total);
    for(size_t i=0;i<np && written<500;i++){
        unsigned char script[64];
        int slen=addr_2_output_script(payouts[i].addr, script, 64);
        if(slen<=0) continue;   // address inválida → su parte cae al pool (leftover)
        memcpy(s->available_coinbase_outputs[written].output_script, script, slen);
        s->available_coinbase_outputs[written].output_script_len=slen;
        s->available_coinbase_outputs[written].value_sats=payouts[i].sats;
        written++;
    }
    DLOG_INFO("CHIRP fill: hv=%d coinbase_value=%llu candidates=%zu payouts=%zu written=%d pool_total=%llu", (s->block_template?s->block_template->header_version:-1), (unsigned long long)s->coinbase_value, nc, np, written, (unsigned long long)pool_total);
    s->available_coinbase_outputs_count=written;
    free(cands);
    pthread_mutex_unlock(&g_lock);
    return written;
}
