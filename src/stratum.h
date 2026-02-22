#ifndef STRATUM_API_H
#define STRATUM_API_H

/*
 * stratum.h — CKB Stratum protocol
 *
 * CKB Stratum differences from Bitcoin:
 *  - mining.notify sends: [job_id, pow_hash (32B hex), target (32B hex), clean_jobs]
 *  - No coinbase, merkle branches, version, nbits, ntime
 *  - 128-bit nonce; we use: bytes 0-7 = 64-bit counter (LE), bytes 8-15 = extranonce1
 *  - mining.submit sends: [worker, job_id, nonce_hex (32 hex chars = 16 bytes)]
 */

#include "cJSON.h"
#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#define HASH_SIZE 32

#define BUFFER_JSON_DOC 4096
#define BUFFER 1024

/* Pool subscription data */
typedef struct {
    String sub_details;
    String extranonce1;       /* pool-assigned prefix, hex string (up to 16 hex chars = 8 bytes) */
    String extranonce2;       /* unused for CKB but kept for compat */
    int    extranonce2_size;
    char   wName[80];
    char   wPass[20];
} mining_subscribe;

/* CKB job (from mining.notify) */
typedef struct {
    String  job_id;
    uint8_t pow_hash[32];    /* 32-byte pow_hash, binary */
    uint8_t target[32];      /* 32-byte target, little-endian binary */
    bool    clean_jobs;
} mining_job;

typedef enum {
    STRATUM_SUCCESS,
    STRATUM_UNKNOWN,
    STRATUM_PARSE_ERROR,
    MINING_NOTIFY,
    MINING_SET_DIFFICULTY,
    MINING_SET_TARGET      /* ViaBTC CKB sends share target as hex, not numeric difficulty */
} stratum_method;

unsigned long getNextId(unsigned long id);
bool verifyPayload(String* line);
bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC> doc);

/* Mining.subscribe */
mining_subscribe init_mining_subscribe(void);
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe);
bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe);

/* Mining.authorize */
bool tx_mining_auth(WiFiClient& client, const char* user, const char* pass);
stratum_method parse_mining_method(String line);

/* Mining.notify — CKB version */
bool parse_mining_notify(String line, mining_job& mJob);

/* Mining.submit — nonce_lo is the 64-bit counter (lower 8 bytes of 128-bit nonce) */
bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob,
                      uint64_t nonce_lo, unsigned long& submit_id);

/* Difficulty helpers */
bool tx_suggest_difficulty(WiFiClient& client, double difficulty);
bool parse_mining_set_difficulty(String line, double& difficulty);
bool parse_mining_set_target(String line, uint8_t target_le[32]);  /* ViaBTC CKB */

unsigned long parse_extract_id(const String& line);

#endif /* STRATUM_API_H */
