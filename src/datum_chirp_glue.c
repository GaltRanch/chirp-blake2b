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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "datum_logger.h"
#include "datum_pow.h"   // datum_blake2b_256 (commitment del snapshot)

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
                        if(m->n_shares==m->cap_shares){
                            size_t ncap=m->cap_shares?m->cap_shares*2:32;
                            chirp_share_t *ns=realloc(m->shares,ncap*sizeof(chirp_share_t));
                            if(!ns) break;   // OOM while loading the registry: keep what we have
                            m->shares=ns; m->cap_shares=ncap;
                        }
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

// ── Snapshot comprometido en el coinbase ─────────────────────────────────────────────────────────
// Canonical JSON (jansson JSON_COMPACT|JSON_SORT_KEYS, SOLO enteros y strings — nunca floats, para que
// Python `json.dumps(obj, sort_keys=True, separators=(",",":"))` produzca los MISMOS bytes) → BLAKE2b-256.
// Los doubles (weight, umbrales) van como bits IEEE754 en hex / strings %.17g: el verificador reusa los bits
// exactos y además recomputa el peso desde active_secs/power con tolerancia.
static const char *chirp_snapshot_dir(void){
    const char *e = getenv("CHIRP_SNAPSHOT_DIR");
    return (e && *e) ? e : CHIRP_SNAPSHOT_DIR_DFLT;
}
static void dbl_bits_hex(double d, char out[19]){ uint64_t u; memcpy(&u,&d,8); snprintf(out,19,"0x%016llx",(unsigned long long)u); }

// Escribe el snapshot (si no existe) y devuelve el hash en *hash32. Poda archivos >48h cada ~200 llamadas.
static bool chirp_snapshot_commit(const T_DATUM_STRATUM_JOB *s, uint64_t now, uint64_t seed,
                                  const chirp_cand_t *cands, size_t nc, const chirp_payout_t *payouts, size_t np,
                                  uint64_t pool_total, double min_days, double min_power, unsigned char hash32[32]){
    char buf[64], ph[65];
    for(int i=0;i<32;i++) snprintf(&ph[i*2],3,"%02x",s->prevhash_bin[31-i]);   // orden display (getblockhash)
    json_t *o=json_object(), *ca=json_array(), *pa=json_array();
    json_object_set_new(o,"v",json_integer(1));
    json_object_set_new(o,"tag",json_string(CHIRP_SNAPSHOT_TAG));
    json_object_set_new(o,"height",json_integer((json_int_t)s->height));
    json_object_set_new(o,"prevhash",json_string(ph));
    snprintf(buf,sizeof(buf),"%llu",(unsigned long long)seed); json_object_set_new(o,"seed",json_string(buf));
    json_object_set_new(o,"ts",json_integer((json_int_t)now));
    json_object_set_new(o,"coinbase_value",json_integer((json_int_t)s->coinbase_value));
    json_object_set_new(o,"fee_bps",json_integer(CHIRP_FEE_BPS));
    json_object_set_new(o,"max_n",json_integer(CHIRP_MAX_N));
    json_object_set_new(o,"min_payout_sats",json_integer((json_int_t)CHIRP_MIN_PAYOUT_SATS));
    double days_full = CHIRP_DAYS_FULL, power_full = CHIRP_POWER_FULL; { const char *e;
      if((e=getenv("CHIRP_DAYS_FULL"))&&*e&&atof(e)>0) days_full=atof(e);
      if((e=getenv("CHIRP_POWER_FULL"))&&*e&&atof(e)>0) power_full=atof(e); }
    snprintf(buf,sizeof(buf),"%.17g",min_days);   json_object_set_new(o,"min_days",json_string(buf));
    snprintf(buf,sizeof(buf),"%.17g",min_power);  json_object_set_new(o,"min_power",json_string(buf));
    snprintf(buf,sizeof(buf),"%.17g",days_full);  json_object_set_new(o,"days_full",json_string(buf));
    snprintf(buf,sizeof(buf),"%.17g",power_full); json_object_set_new(o,"power_full",json_string(buf));
    for(size_t i=0;i<nc;i++){
        json_t *c=json_object(); char wb[19]; dbl_bits_hex(cands[i].weight,wb);
        json_object_set_new(c,"addr",json_string(cands[i].addr));
        json_object_set_new(c,"active_secs",json_integer((json_int_t)cands[i].active_secs));
        json_object_set_new(c,"power",json_integer((json_int_t)cands[i].power));
        json_object_set_new(c,"weight_bits",json_string(wb));
        json_array_append_new(ca,c);
    }
    for(size_t i=0;i<np;i++){
        json_t *p=json_object();
        json_object_set_new(p,"addr",json_string(payouts[i].addr));
        json_object_set_new(p,"sats",json_integer((json_int_t)payouts[i].sats));
        json_array_append_new(pa,p);
    }
    json_object_set_new(o,"candidates",ca);
    json_object_set_new(o,"payouts",pa);
    json_object_set_new(o,"pool_total",json_integer((json_int_t)pool_total));
    char *canon=json_dumps(o,JSON_COMPACT|JSON_SORT_KEYS);
    json_decref(o);
    if(!canon) return false;
    bool ok=datum_blake2b_256(hash32,(const unsigned char*)canon,strlen(canon));
    if(ok){
        char hx[65]; for(int i=0;i<32;i++) snprintf(&hx[i*2],3,"%02x",hash32[i]);
        char path[640], tmp[648]; snprintf(path,sizeof(path),"%s/%s.json",chirp_snapshot_dir(),hx);
        FILE *f=fopen(path,"rb");
        if(f){ fclose(f); }
        else {
            snprintf(tmp,sizeof(tmp),"%s.tmp",path);
            f=fopen(tmp,"wb");
            if(f){ fwrite(canon,1,strlen(canon),f); fclose(f); rename(tmp,path); }
            else DLOG_WARN("CHIRP snapshot: no pude escribir %s (¿existe CHIRP_SNAPSHOT_DIR?)", path);
        }
        static unsigned calls=0;
        if((++calls % 200)==0){   // poda: snapshots > 48h (los de bloques encontrados los archiva aparte el sitio)
            DIR *d=opendir(chirp_snapshot_dir());
            if(d){ struct dirent *de; char p2[640]; struct stat st;
                while((de=readdir(d))){ size_t l=strlen(de->d_name); if(l<6||strcmp(de->d_name+l-5,".json")) continue;
                    snprintf(p2,sizeof(p2),"%s/%s",chirp_snapshot_dir(),de->d_name);
                    if(stat(p2,&st)==0 && now>(uint64_t)st.st_mtime && now-(uint64_t)st.st_mtime>48u*3600u) unlink(p2); }
                closedir(d); }
        }
    }
    free(canon);
    return ok;
}

// Rellena job->available_coinbase_outputs[] con [OP_RETURN commitment, ganador_i ∝ weight …]. El leftover
// (fee 0.9% + dust) lo paga datum al pool_addr automáticamente. seed = primeros 8 bytes de prevhash_bin (LE).
// El output 0 es el commitment del snapshot (0 sats, 40 bytes) para que SIEMPRE quepa. Devuelve #outputs.
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
    if(!cands){ pthread_mutex_unlock(&g_lock); return 0; }
    chirp_payout_t payouts[CHIRP_MAX_N]; uint64_t pool_total=0;
    // umbrales del sindicato: ENV override (para tunear/bootstrap sin recompilar) → fallback a los #define.
    double min_days = CHIRP_MIN_DAYS, min_power = CHIRP_MIN_POWER;
    { const char *e;
      if((e=getenv("CHIRP_MIN_DAYS"))  && *e) min_days  = atof(e);
      if((e=getenv("CHIRP_MIN_POWER")) && *e) min_power = atof(e); }
    size_t nc=chirp_candidates(&g_chirp, now, min_days, min_power, cands, cap);
    size_t np=chirp_split(cands, nc, s->coinbase_value, CHIRP_FEE_BPS, seed, payouts, &pool_total);
    // output 0: OP_RETURN "CHIRP1" || BLAKE2b-256(snapshot)  →  6a 26 <38 bytes>
    unsigned char h32[32]; char hx[65]="";
    if(chirp_snapshot_commit(s, now, seed, cands, nc, payouts, np, pool_total, min_days, min_power, h32)){
        unsigned char *sc=s->available_coinbase_outputs[written].output_script;
        sc[0]=0x6a; sc[1]=6+32; memcpy(&sc[2],CHIRP_SNAPSHOT_TAG,6); memcpy(&sc[8],h32,32);
        s->available_coinbase_outputs[written].output_script_len=2+6+32;
        s->available_coinbase_outputs[written].value_sats=0;
        written++;
        for(int i=0;i<32;i++) snprintf(&hx[i*2],3,"%02x",h32[i]);
    }
    for(size_t i=0;i<np && written<500;i++){
        unsigned char script[64];
        int slen=addr_2_output_script(payouts[i].addr, script, 64);
        if(slen<=0) continue;   // address inválida → su parte cae al pool (leftover)
        memcpy(s->available_coinbase_outputs[written].output_script, script, slen);
        s->available_coinbase_outputs[written].output_script_len=slen;
        s->available_coinbase_outputs[written].value_sats=payouts[i].sats;
        written++;
    }
    DLOG_INFO("CHIRP fill: hv=%d h=%llu coinbase_value=%llu candidates=%zu payouts=%zu written=%d pool_total=%llu snapshot=%s", (s->block_template?s->block_template->header_version:-1), (unsigned long long)s->height, (unsigned long long)s->coinbase_value, nc, np, written, (unsigned long long)pool_total, hx[0]?hx:"none");
    s->available_coinbase_outputs_count=written;
    free(cands);
    pthread_mutex_unlock(&g_lock);
    return written;
}
