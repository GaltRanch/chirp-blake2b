// datum_chirp_glue.h — pegamento CHIRP↔datum: registro global + persistencia + fill de coinbase outputs.
#ifndef DATUM_CHIRP_GLUE_H
#define DATUM_CHIRP_GLUE_H
#include <stdint.h>
struct T_DATUM_STRATUM_JOB;   // fwd
void chirp_glue_init(void);                                   // carga registro de disco
void chirp_glue_record(const char *username, uint64_t diff);  // on-share-aceptado (thread-safe)
void chirp_glue_maybe_save(void);                             // snapshot periódico
// rellena s->available_coinbase_outputs[] con el split ponderado (seed=prevhash). Devuelve #outputs.
int  chirp_glue_fill_outputs(void *job);                      // void* = T_DATUM_STRATUM_JOB*
#endif
