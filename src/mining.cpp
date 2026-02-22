/*
 * mining.cpp — CKB NerdMiner mining core
 *
 * Adapted from NerdMiner_v2 (Bitcoin/SHA256d) for CKB (Eaglesong PoW).
 *
 * Key changes vs original:
 *  - SHA256d + midstate optimisation → Eaglesong sponge hash
 *  - 32-bit nonce → 64-bit nonce counter (lower 8B) + extranonce1 prefix (upper 8B)
 *  - No coinbase / merkle computation (pool sends pow_hash directly)
 *  - Hardware SHA256 acceleration removed
 *  - NONCE_PER_JOB_SW = 65536 (larger slice, Eaglesong ~4× slower)
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "eaglesong/eaglesong.h"
#include "stratum.h"
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "timeconst.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include <mutex>
#include <list>
#include <map>
#include "i2c_master.h"

/* ------------------------------------------------------------------ */
/* Defines                                                             */
/* ------------------------------------------------------------------ */

//#define RANDOM_NONCE

nvs_handle_t stat_handle;

uint32_t templates    = 0;
uint32_t hashes       = 0;
uint32_t Mhashes      = 0;
uint32_t totalKHashes = 0;
uint32_t elapsedKHs   = 0;
uint64_t upTime       = 0;

volatile uint32_t shares; /* increased when hash has 32 bits of zeroes */
volatile uint32_t valids; /* increased when hash <= target              */

double best_diff = 0.0;

extern TSettings Settings;

IPAddress serverIP(1, 1, 1, 1);

static WiFiClient    client;
static miner_data    mMiner;
mining_subscribe     mWorker;
mining_job           mJob;
monitor_data         mMonitor;
static bool volatile isMinerSuscribed = false;

/* ------------------------------------------------------------------ */
/* Strip URI scheme from pool address (e.g. "stratum+tcp://host:port"  */
/* → "host"). Also strips trailing :port if included in the URL.       */
/* ------------------------------------------------------------------ */
static String getCleanHost(const String& addr) {
    String host = addr;
    // Remove scheme: stratum+tcp://, stratum://, tcp://, etc.
    int schemeEnd = host.indexOf("://");
    if (schemeEnd >= 0) host = host.substring(schemeEnd + 3);
    // Strip trailing :port (port is stored separately in Settings.PoolPort)
    int portIdx = host.indexOf(':');
    if (portIdx >= 0) host = host.substring(0, portIdx);
    // Strip trailing slashes
    while (host.length() > 0 && host[host.length()-1] == '/')
        host = host.substring(0, host.length() - 1);
    host.trim();
    return host;
}
unsigned long        mLastTXtoPool    = millis();

int saveIntervals[7]  = {5*60, 15*60, 30*60, 1*3600, 3*3600, 6*3600, 12*3600};
int saveIntervalsSize  = sizeof(saveIntervals) / sizeof(saveIntervals[0]);
int currentIntervalIndex = 0;

/* ------------------------------------------------------------------ */
/* Job queue structures                                                */
/* ------------------------------------------------------------------ */

struct JobRequest {
    uint32_t id;
    uint64_t nonce_start;   /* 64-bit iteration counter start     */
    uint32_t nonce_count;   /* number of nonces to try            */
    double   difficulty;
    uint8_t  pow_hash[32];  /* CKB pow_hash from pool             */
    uint8_t  nonce_prefix[8]; /* extranonce1 → nonce bytes [8..15] */
    uint8_t  target[32];    /* comparison target (LE)             */
};

struct JobResult {
    uint32_t id;
    double   difficulty;
    uint8_t  hash[32];
    uint64_t nonce;         /* 64-bit nonce counter that found the hash */
    uint32_t nonce_count;
    bool     is32bit;
    bool     isValid;
};

struct Submition {
    double diff;
    bool   is32bit;
    bool   isValid;
};

static std::mutex s_job_mutex;
static std::list<std::shared_ptr<JobRequest>> s_job_request_list_sw;
static std::list<std::shared_ptr<JobResult>>  s_job_result_list;
static std::map<uint32_t, std::shared_ptr<Submition>> s_submition_map;

static uint8_t   s_working_current_job_id = 0xFF;
static uint64_t  nonce_pool = 0;

static uint32_t job_pool    = 0xFFFFFFFF;
static uint32_t last_job_time = 0;

/* ------------------------------------------------------------------ */
/* Job helpers                                                         */
/* ------------------------------------------------------------------ */

static void JobPush(std::list<std::shared_ptr<JobRequest>>& job_list,
                    uint32_t id,
                    uint64_t nonce_start, uint32_t nonce_count,
                    double   difficulty,
                    const uint8_t* pow_hash,
                    const uint8_t* nonce_prefix,
                    const uint8_t* target)
{
    auto job           = std::make_shared<JobRequest>();
    job->id            = id;
    job->nonce_start   = nonce_start;
    job->nonce_count   = nonce_count;
    job->difficulty    = difficulty;
    memcpy(job->pow_hash,      pow_hash,      32);
    memcpy(job->nonce_prefix,  nonce_prefix,  8);
    memcpy(job->target,        target,        32);
    job_list.push_back(job);
}

static void MiningJobStop(uint32_t& job_pool,
                          std::map<uint32_t, std::shared_ptr<Submition>>& submition_map)
{
    {
        std::lock_guard<std::mutex> lock(s_job_mutex);
        s_job_result_list.clear();
        s_job_request_list_sw.clear();
    }
    s_working_current_job_id = 0xFF;
    job_pool = 0xFFFFFFFF;
    submition_map.clear();
}

/* ------------------------------------------------------------------ */
/* Pool connection / keepalive                                         */
/* ------------------------------------------------------------------ */

static double currentPoolDifficulty = DEFAULT_DIFFICULTY;

bool checkPoolConnection(void)
{
    if (client.connected()) return true;
    isMinerSuscribed = false;

    String cleanHost = getCleanHost(Settings.PoolAddress);

    Serial.println("[POOL] Client not connected, trying to connect...");
    Serial.printf("[POOL] Address (raw):   %s\n", Settings.PoolAddress.c_str());
    Serial.printf("[POOL] Address (clean): %s\n", cleanHost.c_str());
    Serial.printf("[POOL] Port:            %d\n", Settings.PoolPort);

    if (serverIP == IPAddress(1,1,1,1)) {
        WiFi.hostByName(cleanHost.c_str(), serverIP);
        Serial.printf("[POOL] Resolved DNS (first time): %s\n", serverIP.toString().c_str());
    }

    if (!client.connect(serverIP, Settings.PoolPort)) {
        Serial.printf("[POOL] Cannot connect to: %s:%d — re-resolving DNS\n", cleanHost.c_str(), Settings.PoolPort);
        serverIP = IPAddress(1,1,1,1);   /* force re-resolve next attempt */
        WiFi.hostByName(cleanHost.c_str(), serverIP);
        Serial.printf("[POOL] Resolved DNS: %s\n", serverIP.toString().c_str());
        return false;
    }
    Serial.printf("[POOL] Connected to: %s:%d\n", cleanHost.c_str(), Settings.PoolPort);
    return true;
}

static unsigned long mStart0Hashrate = 0;

bool checkPoolInactivity(unsigned int keepAliveTime, unsigned long inactivityTime)
{
    unsigned long currentKHashes = (Mhashes * 1000) + hashes / 1000;
    unsigned long elapsed = currentKHashes - totalKHashes;

    uint32_t now = millis();

    if (now < mLastTXtoPool) mLastTXtoPool = now;
    if (now > mLastTXtoPool + keepAliveTime) {
        mLastTXtoPool = now;
        Serial.println("  Sending: KeepAlive suggest_difficulty");
        tx_suggest_difficulty(client, DEFAULT_DIFFICULTY);
    }

    if (elapsed == 0) {
        if (mStart0Hashrate == 0) mStart0Hashrate = now;
        if ((now - mStart0Hashrate) > inactivityTime) { mStart0Hashrate = 0; return true; }
        return false;
    }
    mStart0Hashrate = 0;
    return false;
}

/* ------------------------------------------------------------------ */
/* Stratum worker task                                                 */
/* ------------------------------------------------------------------ */

void runStratumWorker(void* name)
{
    Serial.printf("\n[WORKER] Started. Running %s on core %d\n",
                  (char*)name, xPortGetCoreID());

    /* Verify Eaglesong implementation on startup */
    if (eaglesong_selftest()) {
        Serial.println("[WORKER] Eaglesong selftest: PASS ✓");
    } else {
        Serial.println("[WORKER] *** Eaglesong selftest FAILED — halting ***");
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);

    while (1) {
        /* ---- (Re)connect to pool ---- */
        if (!checkPoolConnection()) {
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!isMinerSuscribed) {
            mWorker = init_mining_subscribe();

            if (!tx_mining_subscribe(client, mWorker)) {
                client.stop();
                MiningJobStop(job_pool, s_submition_map);
                continue;
            }

            /* Build full username: "account.worker" if WorkerName set, else just account */
            if (strlen(Settings.WorkerName) > 0) {
                snprintf(mWorker.wName, sizeof(mWorker.wName), "%s.%s",
                         Settings.BtcWallet, Settings.WorkerName);
            } else {
                strncpy(mWorker.wName, Settings.BtcWallet, sizeof(mWorker.wName) - 1);
                mWorker.wName[sizeof(mWorker.wName) - 1] = '\0';
            }
            strcpy(mWorker.wPass, Settings.PoolPassword);
            Serial.printf("[STRATUM] Authorizing as: %s\n", mWorker.wName);
            tx_mining_auth(client, mWorker.wName, mWorker.wPass);
            tx_suggest_difficulty(client, currentPoolDifficulty);

            isMinerSuscribed = true;
            uint32_t t = millis();
            mLastTXtoPool = t;
            last_job_time = t;
        }

        /* ---- Inactivity / timeout checks ---- */
        if (checkPoolInactivity(KEEPALIVE_TIME_ms, POOLINACTIVITY_TIME_ms)) {
            Serial.println("  Pool inactivity detected — reconnecting");
            client.stop();
            isMinerSuscribed = false;
            MiningJobStop(job_pool, s_submition_map);
            continue;
        }

        {
            uint32_t now = millis();
            if (now < last_job_time) last_job_time = now;
            if (now >= last_job_time + 10 * 60 * 1000) {
                client.stop();
                isMinerSuscribed = false;
                MiningJobStop(job_pool, s_submition_map);
                continue;
            }
        }

        /* ---- Read pool messages ---- */
        while (client.connected() && client.available()) {
            String line = client.readStringUntil('\n');
            stratum_method result = parse_mining_method(line);

            switch (result) {

            case MINING_NOTIFY:
                if (parse_mining_notify(line, mJob)) {
                    {
                        std::lock_guard<std::mutex> lock(s_job_mutex);
                        s_job_request_list_sw.clear();
                    }
                    templates++;
                    job_pool++;
                    s_working_current_job_id = job_pool & 0xFF;

                    last_job_time = millis();
                    mLastTXtoPool = last_job_time;

                    uint32_t mh = hashes / 1000000;
                    Mhashes += mh;
                    hashes  -= mh * 1000000;

                    /* CKB: calculateMiningData parses pow_hash, target, extranonce1 */
                    mMiner = calculateMiningData(mWorker, mJob);

                    /* Seed nonce_pool — start from 1, or random */
#ifdef RANDOM_NONCE
                    nonce_pool = ((uint64_t)esp_random() << 32) | esp_random();
                    nonce_pool &= 0xFFFFFFFF00000000ULL; /* keep upper 32 bits random, lower 32 start at 0 */
#else
                    nonce_pool = 1; /* 0 is reserved */
#endif

                    {
                        std::lock_guard<std::mutex> lock(s_job_mutex);
                        for (int i = 0; i < 4; ++i) {
                            JobPush(s_job_request_list_sw,
                                    job_pool,
                                    nonce_pool, NONCE_PER_JOB_SW,
                                    currentPoolDifficulty,
                                    mMiner.pow_hash,
                                    mMiner.nonce_prefix,
                                    mMiner.bytearray_target);
                            nonce_pool += NONCE_PER_JOB_SW;
                        }
                    }
                } else {
                    Serial.println("Parsing error, restarting connection");
                    client.stop();
                    isMinerSuscribed = false;
                    MiningJobStop(job_pool, s_submition_map);
                }
                break;

            case MINING_SET_DIFFICULTY:
                parse_mining_set_difficulty(line, currentPoolDifficulty);
                break;

            case STRATUM_SUCCESS: {
                unsigned long rid = parse_extract_id(line);
                auto it = s_submition_map.find(rid);
                if (it != s_submition_map.end()) {
                    if (it->second->diff > best_diff)
                        best_diff = it->second->diff;
                    if (it->second->is32bit)  shares++;
                    if (it->second->isValid) {
                        Serial.println("CONGRATULATIONS! Valid block found");
                        valids++;
                    }
                    s_submition_map.erase(it);
                }
                break;
            }

            case STRATUM_PARSE_ERROR: {
                unsigned long rid = parse_extract_id(line);
                auto it = s_submition_map.find(rid);
                if (it != s_submition_map.end()) {
                    Serial.printf("Refused submission %lu\n", rid);
                    s_submition_map.erase(it);
                }
                break;
            }

            default:
                Serial.println("  Parsed JSON: unknown method");
                break;
            }
        }

        /* ---- Collect results from miner tasks ---- */
        vTaskDelay(50 / portTICK_PERIOD_MS);

        std::list<std::shared_ptr<JobResult>> job_result_list;

        {
            std::lock_guard<std::mutex> lock(s_job_mutex);
            job_result_list.insert(job_result_list.end(),
                                   s_job_result_list.begin(),
                                   s_job_result_list.end());
            s_job_result_list.clear();

            /* Replenish SW job queue */
            while (s_job_request_list_sw.size() < 4) {
                JobPush(s_job_request_list_sw,
                        job_pool,
                        nonce_pool, NONCE_PER_JOB_SW,
                        currentPoolDifficulty,
                        mMiner.pow_hash,
                        mMiner.nonce_prefix,
                        mMiner.bytearray_target);
                nonce_pool += NONCE_PER_JOB_SW;
            }
        }

        /* ---- Submit shares ---- */
        while (!job_result_list.empty()) {
            auto res = job_result_list.front();
            job_result_list.pop_front();

            hashes += res->nonce_count;

            if (res->difficulty > currentPoolDifficulty &&
                job_pool == res->id &&
                res->nonce != 0xFFFFFFFFFFFFFFFFULL)
            {
                if (!client.connected()) break;

                unsigned long submit_id = 0;
                tx_mining_submit(client, mWorker, mJob, res->nonce, submit_id);

                Serial.print("   - Current diff share: "); Serial.println(res->difficulty, 12);
                Serial.print("   - Current pool diff : "); Serial.println(currentPoolDifficulty, 12);
                Serial.print("   - TX SHARE hash: ");
                for (int i = 0; i < 32; i++) Serial.printf("%02x", res->hash[i]);
                Serial.println();

                mLastTXtoPool = millis();

                auto sub    = std::make_shared<Submition>();
                sub->diff   = res->difficulty;
                /* For CKB: "32-bit share" ≈ hash MSByte (LE index 31) and next byte are 0 */
                sub->is32bit = (res->hash[31] == 0 && res->hash[30] == 0);
                sub->isValid = checkValid(res->hash, mMiner.bytearray_target);

                s_submition_map.insert(std::make_pair(submit_id, sub));
                if (s_submition_map.size() > 32)
                    s_submition_map.erase(s_submition_map.begin());
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Miner worker (software Eaglesong)                                  */
/* ------------------------------------------------------------------ */

void minerWorkerSw(void* task_id)
{
    unsigned int miner_id = (uint32_t)task_id;
    Serial.printf("[MINER] %u Started minerWorkerSw (Eaglesong/CKB)\n", miner_id);

    std::shared_ptr<JobRequest> job;
    std::shared_ptr<JobResult>  result;

    /* 48-byte Eaglesong input: pow_hash[32] || nonce[16] */
    uint8_t input[48];
    uint8_t hash[32];
    uint32_t wdt_counter = 0;

    while (1) {
        /* ---- Publish result, fetch next job ---- */
        {
            std::lock_guard<std::mutex> lock(s_job_mutex);
            if (result) {
                if (s_job_result_list.size() < 16)
                    s_job_result_list.push_back(result);
                result.reset();
            }
            if (!s_job_request_list_sw.empty()) {
                job = s_job_request_list_sw.front();
                s_job_request_list_sw.pop_front();
            } else {
                job.reset();
            }
        }

        if (job) {
            result = std::make_shared<JobResult>();
            result->difficulty  = job->difficulty;
            result->nonce       = 0xFFFFFFFFFFFFFFFFULL;
            result->id          = job->id;
            result->nonce_count = job->nonce_count;

            uint8_t job_in_work = job->id & 0xFF;

            /*
             * Build constant parts of the 48-byte input once:
             *   input[ 0..31] = pow_hash  (fixed for this job)
             *   input[32..39] = nonce bytes 0-7  (64-bit counter, LE) — updated per nonce
             *   input[40..47] = nonce bytes 8-15 (extranonce1 prefix) — fixed for this job
             */
            memcpy(input,      job->pow_hash,     32);
            memcpy(input + 40, job->nonce_prefix,  8);

            /* DEBUG: print first hash of each job on miner 0 */
            bool printed_first_hash = false;

            for (uint32_t n = 0; n < job->nonce_count; ++n) {
                uint64_t nv = job->nonce_start + (uint64_t)n;

                /* Store nonce counter LE into input[32..39] */
                input[32] = (uint8_t)(nv);
                input[33] = (uint8_t)(nv >> 8);
                input[34] = (uint8_t)(nv >> 16);
                input[35] = (uint8_t)(nv >> 24);
                input[36] = (uint8_t)(nv >> 32);
                input[37] = (uint8_t)(nv >> 40);
                input[38] = (uint8_t)(nv >> 48);
                input[39] = (uint8_t)(nv >> 56);

                eaglesong(input, 48, hash, 32);

                /* DEBUG: print first hash of each new job (miner_id==0 only) */
                if (!printed_first_hash && miner_id == 0) {
                    printed_first_hash = true;
                    Serial.printf("[MINER] Job %u nonce0=0x%016llx\n", job->id, nv);
                    Serial.print("[MINER] input[0..31]  (pow_hash): ");
                    for (int i=0; i<32; i++) Serial.printf("%02x", input[i]);
                    Serial.println();
                    Serial.print("[MINER] input[32..47] (nonce LE): ");
                    for (int i=32; i<48; i++) Serial.printf("%02x", input[i]);
                    Serial.println();
                    Serial.print("[MINER] hash:                     ");
                    for (int i=0; i<32; i++) Serial.printf("%02x", hash[i]);
                    Serial.println();
                    Serial.printf("[MINER] hash diff: %.6f  pool diff: %.6f\n",
                                  diff_from_target(hash), job->difficulty);
                }

                double diff_hash = diff_from_target(hash);
                if (diff_hash > result->difficulty) {
                    result->difficulty = diff_hash;
                    result->nonce      = nv;
                    memcpy(result->hash, hash, 32);
                }

                /* Check for job abort every 256 nonces */
                if (((uint16_t)(n & 0xFF) == 0) &&
                    (s_working_current_job_id != job_in_work))
                {
                    result->nonce_count = n + 1;
                    break;
                }
            }
        } else {
            vTaskDelay(2 / portTICK_PERIOD_MS);
        }

        if (++wdt_counter >= 8) {
            wdt_counter = 0;
            esp_task_wdt_reset();
        }
    }
}

/* ------------------------------------------------------------------ */
/* NVS stats persistence (unchanged from NerdMiner_v2)               */
/* ------------------------------------------------------------------ */

#define DELAY        100
#define REDRAW_EVERY 10

void restoreStat() {
    if (!Settings.saveStats) return;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        nvs_flash_init();

    nvs_open("state", NVS_READWRITE, &stat_handle);

    size_t required_size = sizeof(double);
    nvs_get_blob(stat_handle, "best_diff", &best_diff, &required_size);
    nvs_get_u32(stat_handle, "Mhashes",   &Mhashes);
    uint32_t nv_shares = 0, nv_valids = 0;
    nvs_get_u32(stat_handle, "shares",    &nv_shares);
    nvs_get_u32(stat_handle, "valids",    &nv_valids);
    shares = nv_shares;
    valids = nv_valids;
    nvs_get_u32(stat_handle, "templates", &templates);
    nvs_get_u64(stat_handle, "upTime",    &upTime);

    uint32_t crc = crc32_reset();
    crc = crc32_add(crc, &best_diff,  sizeof(best_diff));
    crc = crc32_add(crc, &Mhashes,   sizeof(Mhashes));
    crc = crc32_add(crc, &nv_shares,  sizeof(nv_shares));
    crc = crc32_add(crc, &nv_valids,  sizeof(nv_valids));
    crc = crc32_add(crc, &templates,  sizeof(templates));
    crc = crc32_add(crc, &upTime,     sizeof(upTime));
    crc = crc32_finish(crc);

    uint32_t nv_crc;
    nvs_get_u32(stat_handle, "crc32", &nv_crc);
    if (nv_crc != crc) {
        best_diff = 0.0;
        Mhashes = shares = valids = templates = 0;
        upTime = 0;
    }
}

void saveStat() {
    if (!Settings.saveStats) return;
    Serial.printf("[MONITOR] Saving stats\n");
    nvs_set_blob(stat_handle, "best_diff", &best_diff, sizeof(best_diff));
    nvs_set_u32(stat_handle, "Mhashes",   Mhashes);
    nvs_set_u32(stat_handle, "shares",    shares);
    nvs_set_u32(stat_handle, "valids",    valids);
    nvs_set_u32(stat_handle, "templates", templates);
    nvs_set_u64(stat_handle, "upTime",    upTime);

    uint32_t nv_shares = shares, nv_valids = valids;
    uint32_t crc = crc32_reset();
    crc = crc32_add(crc, &best_diff,  sizeof(best_diff));
    crc = crc32_add(crc, &Mhashes,   sizeof(Mhashes));
    crc = crc32_add(crc, &nv_shares,  sizeof(nv_shares));
    crc = crc32_add(crc, &nv_valids,  sizeof(nv_valids));
    crc = crc32_add(crc, &templates,  sizeof(templates));
    crc = crc32_add(crc, &upTime,     sizeof(upTime));
    crc = crc32_finish(crc);
    nvs_set_u32(stat_handle, "crc32", crc);
}

void resetStat() {
    Serial.printf("[MONITOR] Resetting NVS stats\n");
    templates = hashes = Mhashes = totalKHashes = elapsedKHs = shares = valids = 0;
    upTime = 0;
    best_diff = 0.0;
    saveStat();
}

/* ------------------------------------------------------------------ */
/* Monitor task (unchanged)                                           */
/* ------------------------------------------------------------------ */

void runMonitor(void* name)
{
    Serial.println("[MONITOR] started");
    restoreStat();

    unsigned long mLastCheck = 0;
    resetToFirstScreen();

    unsigned long frame = 0;
    uint32_t seconds_elapsed = 0;

    totalKHashes = (Mhashes * 1000) + hashes / 1000;
    uint32_t last_update_millis = millis();
    uint32_t uptime_frac = 0;

    while (1) {
        uint32_t now_millis = millis();
        if (now_millis < last_update_millis) now_millis = last_update_millis;

        uint32_t mElapsed = now_millis - mLastCheck;
        if (mElapsed >= 1000) {
            mLastCheck        = now_millis;
            last_update_millis = now_millis;

            unsigned long currentKHashes = (Mhashes * 1000) + hashes / 1000;
            elapsedKHs   = currentKHashes - totalKHashes;
            totalKHashes = currentKHashes;

            uptime_frac += mElapsed;
            while (uptime_frac >= 1000) { uptime_frac -= 1000; upTime++; }

            drawCurrentScreen(mElapsed);

            if (elapsedKHs == 0) {
                Serial.printf(">>> [i] Miner: subscribed>%s / connected>%s / wifi>%s\n",
                    isMinerSuscribed ? "yes" : "no",
                    client.connected() ? "yes" : "no",
                    WiFi.status() == WL_CONNECTED ? "yes" : "no");
            }

#ifdef DEBUG_MEMORY
            Serial.printf("### Heap: %d / %d / %d\n",
                ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
#endif

            seconds_elapsed++;
            if (seconds_elapsed % saveIntervals[currentIntervalIndex] == 0) {
                saveStat();
                seconds_elapsed = 0;
                if (currentIntervalIndex < saveIntervalsSize - 1)
                    currentIntervalIndex++;
            }
        }

        animateCurrentScreen(frame);
        doLedStuff(frame);
        vTaskDelay(DELAY / portTICK_PERIOD_MS);
        frame++;
    }
}
