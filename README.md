# NerdMiner_CKB 🐧⛏️

> **ESP32 solo lottery miner for Nervos Network (CKB) — Eaglesong PoW**

A fork of [NerdMiner_v2](https://github.com/BitMaker-hub/NerdMiner_v2) adapted for **CKB (Nervos Network)** using the **Eaglesong** proof-of-work hash function.

---

## What Changed From NerdMiner_v2

| Component | Original (Bitcoin) | This Fork (CKB) |
|-----------|-------------------|-----------------|
| Hash function | SHA256d + midstate opt | Eaglesong sponge |
| Nonce width | 32-bit | 128-bit (64-bit counter + extranonce1) |
| Stratum notify | job + coinbase + merkle | job_id + pow_hash + target |
| Block header | 80 bytes | Not needed (pool sends pow_hash) |
| Hardware accel | ESP32 SHA256 DMA | Not applicable |

---

## Eaglesong Algorithm

Eaglesong is a sponge-construction hash designed by the Nervos Network for CKB:

- **State**: 16 × uint32_t (512 bits)
- **Rate**: 256 bits (8 words absorbed/squeezed per block)
- **Rounds**: 43 per permutation
- **Delimiter**: 0x06
- **Input byte order**: Big-endian pack into u32
- **Output byte order**: Little-endian extract from u32

### CKB Mining
```
input  = pow_hash[32 bytes] || nonce[16 bytes LE]
output = eaglesong(input) → 32 bytes
valid  = output (LE) <= target (LE)
```

### 128-bit Nonce Layout
```
nonce[0..7]  = 64-bit iteration counter (LE) — miner iterates this
nonce[8..15] = extranonce1 from pool (LE) — pool-assigned prefix
```

---

## Stratum Protocol (CKB)

### Subscribe
Standard `mining.subscribe` + `mining.authorize`.

### Notify
```json
{
  "method": "mining.notify",
  "params": ["job_id", "pow_hash_hex_64chars", "target_hex_64chars", true]
}
```

### Submit
```json
{
  "method": "mining.submit",
  "params": ["worker_name", "job_id", "nonce_hex_32chars"]
}
```

---

## Building

**Requirements**: PlatformIO, ESP32 Arduino framework

```bash
git clone https://github.com/toastmanAu/NerdMiner_CKB
cd NerdMiner_CKB
pio run -e NerdminerV2   # or your target board
pio run -e NerdminerV2 -t upload
```

## Self-Test

On startup, `runStratumWorker` runs `eaglesong_selftest()` which verifies:
- `eaglesong("")` → `9e4452fc7aed93d7240b7b55263792befd1be09252b456401122ba71a56f62a0`
- `eaglesong("1111...\\n")` (35 bytes) → `a50a3310f78cbaeadcffe2d46262119eeeda9d6568b4df1b636399742c867aca`

If the selftest fails, the stratum task halts with an error message on Serial.

---

## Pool Configuration

Set your CKB pool in the NerdMiner WiFi config portal:

- **Pool address**: your CKB Stratum pool (e.g. `ckb.f2pool.com`)
- **Pool port**: pool's Stratum port
- **Wallet**: your CKB `ckb1...` address (used as worker name)

---

## Status

- [x] Eaglesong C implementation (verified against reference vectors)
- [x] CKB Stratum protocol (mining.notify / mining.submit)
- [x] 128-bit nonce with extranonce1 support
- [x] minerWorkerSw adapted for Eaglesong
- [x] Hardware SHA256 acceleration removed
- [x] Worker name field added to WiFi config portal
- [x] Tested against live CKB Stratum pool (ViaBTC)
- [ ] Display labels still reference BTC (cosmetic — monitor.cpp)
- [ ] Performance benchmarking on ESP32-S3

---

## Credits

- Original NerdMiner_v2: [BitMaker-hub](https://github.com/BitMaker-hub/NerdMiner_v2)
- Eaglesong reference: [nervosnetwork/eaglesong](https://github.com/nervosnetwork/eaglesong)
- CKB adaptation: [toastmanAu](https://github.com/toastmanAu)
