/*
 * screenshot.h — HTTP screenshot server for NerdMiner CKB
 * =========================================================
 * Serves the current display frame as a JPEG over WiFi.
 * Enabled only when SCREENSHOT_SERVER is defined (dev-screenshot branch).
 *
 * Usage:
 *   1. Flash with -DSCREENSHOT_SERVER build flag
 *   2. Check Serial for "Screenshot server: http://<ip>/screenshot"
 *   3. Open that URL in a browser or curl it — get a JPEG of the display
 *
 * Endpoints:
 *   GET /screenshot   — JPEG of current display frame
 *   GET /             — Basic status page with clickable screenshot link
 *
 * The sprite buffer (TFT_eSprite background) is read directly —
 * no SPI readback, no display disruption, no artifacts.
 * 
 * Part of NerdMiner_CKB dev-screenshot branch.
 * Not compiled in production builds.
 */

#pragma once

#ifdef SCREENSHOT_SERVER

#include <Arduino.h>
#include <WebServer.h>
#include <TFT_eSPI.h>

// Forward-declare the sprite defined in esp23_2432s028r.cpp
extern TFT_eSprite background;

void initScreenshotServer();
void handleScreenshotServer();

#endif // SCREENSHOT_SERVER
