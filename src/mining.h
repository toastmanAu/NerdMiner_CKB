#ifndef MINING_API_H
#define MINING_API_H

/*
 * mining.h — CKB NerdMiner mining definitions
 *
 * Key CKB differences:
 *  - Hash function: Eaglesong (not SHA256d)
 *  - Nonce: 128-bit (not 32-bit): bytes 0-7 = counter, bytes 8-15 = extranonce1
 *  - No midstate optimisation (Eaglesong is a sponge, not Merkle-Damgård)
 *  - No coinbase / merkle computation (pool sends pow_hash directly)
 */

/* Job slice size — larger than Bitcoin since Eaglesong ~4× slower than SHA256d on ESP32 */
#define NONCE_PER_JOB_SW        65536U

/* Current pool share difficulty — set by mining.set_target or mining.set_difficulty */
extern double currentPoolDifficulty;

#define DEFAULT_DIFFICULTY      0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000

void runMonitor(void* name);
void runStratumWorker(void* name);
void runMiner(void* name);
void minerWorkerSw(void* task_id);

String printLocalTime(void);
void   resetStat(void);

/* ------------------------------------------------------------------ */
/* CKB miner working data                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t bytearray_target[32];   /* current job target (LE, from pool) */
    uint8_t pow_hash[32];           /* current job pow_hash (from pool)    */
    uint8_t nonce_prefix[8];        /* extranonce1 bytes → nonce[8..15]    */
} miner_data;

#endif /* MINING_API_H */
