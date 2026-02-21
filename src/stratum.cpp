/*
 * stratum.cpp — CKB Stratum protocol implementation
 *
 * CKB mining.notify params: [job_id, pow_hash_hex, target_hex, clean_jobs]
 * CKB mining.submit params: [worker, job_id, nonce_hex_32chars]
 *
 * 128-bit nonce layout (little-endian):
 *   bytes  0– 7 = 64-bit counter (LE), iterated by miner
 *   bytes  8–15 = extranonce1 bytes from pool (up to 8 bytes, zero-padded)
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "stratum.h"
#include "cJSON.h"
#include "utils.h"
#include "version.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

static StaticJsonDocument<BUFFER_JSON_DOC> doc;
static unsigned long id = 1;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

unsigned long getNextId(unsigned long id) {
    if (id == ULONG_MAX) { return 1; }
    return id + 1;
}

bool verifyPayload(String* line) {
    if (line->length() == 0) return false;
    line->trim();
    return !line->isEmpty();
}

bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC> doc) {
    if (!doc.containsKey("error")) return false;
    if (doc["error"].size() == 0)  return false;
    Serial.printf("ERROR: %d | reason: %s\n",
                  (const int)doc["error"][0],
                  (const char*)doc["error"][1]);
    return true;
}

/* ------------------------------------------------------------------ */
/* Parse a hex string into a binary buffer                            */
/* ------------------------------------------------------------------ */

static bool hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned v;
        if (sscanf(hex + i * 2, "%02x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* STEP 1: Subscribe                                                  */
/* ------------------------------------------------------------------ */

bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe) {
    char payload[BUFFER] = {0};
    id = 1;
    snprintf(payload, sizeof(payload),
             "{\"id\": %lu, \"method\": \"mining.subscribe\", \"params\": [\"NerdMinerCKB/%s\"]}\n",
             id, CURRENT_VERSION);

    Serial.printf("[WORKER] ==> Mining subscribe\n");
    Serial.print("  Sending  : "); Serial.println(payload);
    client.print(payload);

    vTaskDelay(200 / portTICK_PERIOD_MS);

    String line = client.readStringUntil('\n');
    return parse_mining_subscribe(line, mSubscribe);
}

bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe) {
    if (!verifyPayload(&line)) return false;
    Serial.print("  Receiving: "); Serial.println(line);

    DeserializationError error = deserializeJson(doc, line);
    if (error || checkError(doc)) return false;
    if (!doc.containsKey("result")) return false;

    mSubscribe.sub_details   = String((const char*)doc["result"][0][0][1]);
    mSubscribe.extranonce1   = String((const char*)doc["result"][1]);
    mSubscribe.extranonce2_size = doc["result"][2];

    Serial.print("    sub_details: "); Serial.println(mSubscribe.sub_details);
    Serial.print("    extranonce1: "); Serial.println(mSubscribe.extranonce1);
    Serial.print("    extranonce2_size: "); Serial.println(mSubscribe.extranonce2_size);

    if (mSubscribe.extranonce1.length() == 0) {
        Serial.printf("[WORKER] extranonce1 empty — aborting\n");
        return false;
    }
    return true;
}

mining_subscribe init_mining_subscribe(void) {
    mining_subscribe s;
    s.extranonce1      = "";
    s.extranonce2      = "";
    s.extranonce2_size = 0;
    s.sub_details      = "";
    return s;
}

/* ------------------------------------------------------------------ */
/* STEP 2: Authorize                                                  */
/* ------------------------------------------------------------------ */

bool tx_mining_auth(WiFiClient& client, const char* user, const char* pass) {
    char payload[BUFFER] = {0};
    id = getNextId(id);
    snprintf(payload, sizeof(payload),
             "{\"params\": [\"%s\", \"%s\"], \"id\": %lu, \"method\": \"mining.authorize\"}\n",
             user, pass, id);

    Serial.printf("[WORKER] ==> Authorise work\n");
    Serial.print("  Sending  : "); Serial.println(payload);
    client.print(payload);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    return true;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */

stratum_method parse_mining_method(String line) {
    if (!verifyPayload(&line)) return STRATUM_PARSE_ERROR;
    Serial.print("  Receiving: "); Serial.println(line);

    DeserializationError error = deserializeJson(doc, line);
    if (error || checkError(doc)) return STRATUM_PARSE_ERROR;

    if (!doc.containsKey("method")) {
        return doc["error"].isNull() ? STRATUM_SUCCESS : STRATUM_UNKNOWN;
    }

    const char* method = doc["method"];
    if (strcmp(method, "mining.notify")         == 0) return MINING_NOTIFY;
    if (strcmp(method, "mining.set_difficulty") == 0) return MINING_SET_DIFFICULTY;
    return STRATUM_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* CKB mining.notify                                                  */
/* params: [job_id, pow_hash_hex (64 chars), target_hex (64 chars),  */
/*          clean_jobs (bool)]                                        */
/* ------------------------------------------------------------------ */

bool parse_mining_notify(String line, mining_job& mJob) {
    Serial.println("    Parsing Method [MINING NOTIFY]");
    if (!verifyPayload(&line)) return false;

    DeserializationError error = deserializeJson(doc, line);
    if (error) return false;
    if (!doc.containsKey("params")) return false;

    mJob.job_id     = String((const char*)doc["params"][0]);
    const char* pow_hash_hex = (const char*)doc["params"][1];
    const char* target_hex   = (const char*)doc["params"][2];
    mJob.clean_jobs = doc["params"][3];

    /* Decode hex → binary */
    if (!hex_to_bytes(pow_hash_hex, mJob.pow_hash, 32)) {
        Serial.println("    [ERROR] bad pow_hash hex");
        return false;
    }
    if (!hex_to_bytes(target_hex, mJob.target, 32)) {
        Serial.println("    [ERROR] bad target hex");
        return false;
    }

    Serial.print("    job_id:   "); Serial.println(mJob.job_id);
    Serial.print("    pow_hash: "); Serial.println(pow_hash_hex);
    Serial.print("    target:   "); Serial.println(target_hex);
    Serial.print("    clean:    "); Serial.println(mJob.clean_jobs);

    return !checkError(doc);
}

/* ------------------------------------------------------------------ */
/* CKB mining.submit                                                  */
/* nonce layout: [nonce_lo 8B LE][extranonce1 up to 8B]              */
/* submitted as 32 hex chars                                          */
/* ------------------------------------------------------------------ */

bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob,
                      uint64_t nonce_lo, unsigned long& submit_id)
{
    /* Build 16-byte nonce */
    uint8_t nonce16[16];
    memset(nonce16, 0, sizeof(nonce16));

    /* bytes 0-7: 64-bit counter, little-endian */
    for (int i = 0; i < 8; ++i)
        nonce16[i] = (uint8_t)((nonce_lo >> (8 * i)) & 0xFF);

    /* bytes 8-15: extranonce1 from pool (hex string → bytes, up to 8 bytes) */
    const char* en1 = mWorker.extranonce1.c_str();
    size_t en1_hex_len = strlen(en1);
    size_t en1_bytes = en1_hex_len / 2;
    if (en1_bytes > 8) en1_bytes = 8;
    for (size_t i = 0; i < en1_bytes; ++i) {
        unsigned v;
        if (sscanf(en1 + i * 2, "%02x", &v) == 1)
            nonce16[8 + i] = (uint8_t)v;
    }

    /* Format as 32 hex chars */
    char nonce_hex[33];
    for (int i = 0; i < 16; ++i)
        snprintf(nonce_hex + i * 2, 3, "%02x", nonce16[i]);
    nonce_hex[32] = '\0';

    id = getNextId(id);
    submit_id = id;

    char payload[BUFFER] = {0};
    snprintf(payload, sizeof(payload),
             "{\"id\":%lu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\"]}\n",
             id,
             mWorker.wName,
             mJob.job_id.c_str(),
             nonce_hex);

    Serial.print("  Sending  : "); Serial.print(payload);
    client.print(payload);
    return true;
}

/* ------------------------------------------------------------------ */
/* Difficulty                                                         */
/* ------------------------------------------------------------------ */

bool parse_mining_set_difficulty(String line, double& difficulty) {
    Serial.println("    Parsing Method [SET DIFFICULTY]");
    if (!verifyPayload(&line)) return false;

    DeserializationError error = deserializeJson(doc, line);
    if (error) return false;
    if (!doc.containsKey("params")) return false;

    difficulty = (double)doc["params"][0];
    Serial.print("    difficulty: "); Serial.println(difficulty, 12);
    return true;
}

bool tx_suggest_difficulty(WiFiClient& client, double difficulty) {
    char payload[BUFFER] = {0};
    id = getNextId(id);
    snprintf(payload, sizeof(payload),
             "{\"id\":%lu,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n",
             id, difficulty);
    Serial.print("  Sending  : "); Serial.print(payload);
    return client.print(payload);
}

unsigned long parse_extract_id(const String& line) {
    DeserializationError error = deserializeJson(doc, line);
    if (error || !doc.containsKey("id")) return 0;
    return (unsigned long)doc["id"];
}
