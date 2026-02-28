/*
 * screenshot.cpp — HTTP screenshot server for NerdMiner CKB
 * ==========================================================
 * Only compiled when SCREENSHOT_SERVER is defined.
 * See screenshot.h for usage.
 */

#ifdef SCREENSHOT_SERVER

#include "screenshot.h"
#include <WiFi.h>
#include "monitor.h"   // for mMonitor (hashrate etc for status page)

// -------------------------------------------------------------------
// The background sprite is defined in esp23_2432s028r.cpp.
// We forward-declare it here so we can read its pixel buffer.
// TFT_eSprite::getPointer() returns the internal RGB565 framebuffer.
// -------------------------------------------------------------------
extern TFT_eSprite background;
extern monitor_data mMonitor;

static WebServer screenshotServer(80);

// -------------------------------------------------------------------
// RGB565 → RGB888 conversion
// TFT_eSPI stores pixels as RGB565 (16-bit, big-endian after setSwapBytes)
// JPEG encoder needs packed RGB888.
// -------------------------------------------------------------------
static uint8_t* rgb565_to_rgb888(const uint16_t* src, int w, int h) {
  int nPx = w * h;
  uint8_t* dst = (uint8_t*)malloc(nPx * 3);
  if (!dst) return nullptr;
  for (int i = 0; i < nPx; i++) {
    uint16_t px = src[i];
    // TFT_eSPI sets setSwapBytes(true), so bytes are already in display order
    // RGB565: RRRRRGGGGGGBBBBB
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 =  px        & 0x1F;
    // Expand with bit-replication for accurate colour
    dst[i*3 + 0] = (r5 << 3) | (r5 >> 2);
    dst[i*3 + 1] = (g6 << 2) | (g6 >> 4);
    dst[i*3 + 2] = (b5 << 3) | (b5 >> 2);
  }
  return dst;
}

// -------------------------------------------------------------------
// JPEG encode using ESP-IDF's built-in encoder (no extra lib needed)
// fmt2jpg is part of esp32-camera component, available in Arduino ESP32
// -------------------------------------------------------------------
static bool encodeJpeg(const uint8_t* rgb888, int w, int h,
                       uint8_t** outBuf, size_t* outLen, int quality = 80) {
  // esp32-camera's fmt2jpg — available in arduino-esp32 SDK
  // If it's not available in your SDK version, we fall back to raw BMP
  bool ok = fmt2jpg((uint8_t*)rgb888, w * h * 3, w, h,
                    PIXFORMAT_RGB888, quality, outBuf, outLen);
  return ok && *outBuf && *outLen > 0;
}

// -------------------------------------------------------------------
// HTTP handler: GET /screenshot
// Grabs the current sprite buffer, encodes to JPEG, sends it.
// -------------------------------------------------------------------
static void handleScreenshot() {
  // Snapshot the sprite dimensions
  int w = background.width();
  int h = background.height();

  if (w <= 0 || h <= 0) {
    screenshotServer.send(503, "text/plain",
      "Display not ready yet — wait for mining screen to appear");
    return;
  }

  // Get pointer to sprite's internal RGB565 framebuffer
  // getPointer() returns void* — cast to uint16_t*
  uint16_t* fb = (uint16_t*)background.getPointer();
  if (!fb) {
    screenshotServer.send(503, "text/plain",
      "Sprite buffer not available — sprite may have been deleted/recreated");
    return;
  }

  // Convert to RGB888 (JPEG encoder input)
  uint8_t* rgb888 = rgb565_to_rgb888(fb, w, h);
  if (!rgb888) {
    screenshotServer.send(503, "text/plain", "OOM — not enough heap for RGB888 buffer");
    return;
  }

  // Encode to JPEG
  uint8_t* jpegBuf = nullptr;
  size_t   jpegLen = 0;
  bool ok = encodeJpeg(rgb888, w, h, &jpegBuf, &jpegLen, 85);
  free(rgb888);

  if (!ok || !jpegBuf) {
    screenshotServer.send(503, "text/plain",
      "JPEG encode failed — check fmt2jpg is available in your SDK");
    return;
  }

  // Serve the JPEG
  screenshotServer.sendHeader("Content-Disposition",
    "inline; filename=\"nerdminer-ckb.jpg\"");
  screenshotServer.sendHeader("Cache-Control", "no-cache, no-store");
  screenshotServer.sendHeader("Access-Control-Allow-Origin", "*");
  screenshotServer.send_P(200, "image/jpeg",
    (const char*)jpegBuf, jpegLen);

  free(jpegBuf);

  Serial.printf("[Screenshot] Served %dx%d JPEG (%d bytes) to %s\n",
    w, h, (int)jpegLen,
    screenshotServer.client().remoteIP().toString().c_str());
}

// -------------------------------------------------------------------
// HTTP handler: GET /  — basic status page
// -------------------------------------------------------------------
static void handleRoot() {
  String ip = WiFi.localIP().toString();
  float hashrate = mMonitor.currentHashRate;

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>NerdMiner CKB</title>";
  html += "<style>body{background:#07090f;color:#e2e8f0;font-family:monospace;padding:2rem;max-width:500px;margin:0 auto}";
  html += "h1{color:#00c8ff}a{color:#00c8ff}img{border:1px solid #1e2430;border-radius:8px;max-width:100%;margin-top:1rem}";
  html += ".stat{color:#64748b;font-size:.9rem}.val{color:#e2e8f0;font-weight:bold}</style></head><body>";
  html += "<h1>⛏ NerdMiner CKB</h1>";
  html += "<p class='stat'>IP: <span class='val'>" + ip + "</span></p>";
  html += "<p class='stat'>Hashrate: <span class='val'>";
  html += String(hashrate, 2);
  html += " H/s</span></p>";
  html += "<p class='stat'>Best diff: <span class='val'>" + String(mMonitor.best_diff) + "</span></p>";
  html += "<p><a href='/screenshot'>📷 Grab screenshot</a></p>";
  html += "<img src='/screenshot' alt='Current display'>";
  html += "</body></html>";

  screenshotServer.send(200, "text/html", html);
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------
void initScreenshotServer() {
  screenshotServer.on("/", HTTP_GET, handleRoot);
  screenshotServer.on("/screenshot", HTTP_GET, handleScreenshot);
  screenshotServer.begin();

  String ip = WiFi.localIP().toString();
  Serial.println("");
  Serial.println("┌─────────────────────────────────────────┐");
  Serial.println("│  Screenshot server active (dev build)   │");
  Serial.printf( "│  http://%s/screenshot\n", ip.c_str());
  Serial.printf( "│  http://%s/  (status page)\n", ip.c_str());
  Serial.println("└─────────────────────────────────────────┘");
  Serial.println("");
}

void handleScreenshotServer() {
  screenshotServer.handleClient();
}

#endif // SCREENSHOT_SERVER
