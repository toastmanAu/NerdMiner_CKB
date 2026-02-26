// ckb_config.h — NerdMiner CKB edition
// Implements the CKBCFG serial protocol, writing to SPIFFS /config.json
// compatible with NerdMiner's existing WiFiManager config system.
// Supported JSON keys: SSID, WifiPW, PoolUrl, PoolPassword, BtcWallet, PoolPort
//
// Protocol:
//   Browser sends: CKBCFG\n
//   Device replies: READY:nerdminer-cyd\n
//   Browser sends: { "ssid":"x","pass":"x","url":"x","port":3333,"wallet":"x" }\nEND\n
//   Device replies: OK\n  then reboots

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "SPIFFS.h"
#include "drivers/storage/storage.h"

#define CKB_BOARD_ID "nerdminer-cyd"
#define CKB_CONFIG_TIMEOUT_MS 5000

static void ckb_config_check() {
  unsigned long start = millis();
  String buf = "";
  bool triggered = false;

  // Wait up to 5s for CKBCFG trigger
  while (millis() - start < CKB_CONFIG_TIMEOUT_MS) {
    while (Serial.available()) {
      char c = Serial.read();
      buf += c;
      if (buf.endsWith("CKBCFG\n") || buf.endsWith("CKBCFG\r\n")) {
        triggered = true;
        break;
      }
      if (buf.length() > 32) buf = buf.substring(buf.length() - 32);
    }
    if (triggered) break;
    delay(10);
  }

  if (!triggered) return;

  Serial.println("READY:" CKB_BOARD_ID);
  Serial.flush();

  // Collect JSON until END marker
  String json = "";
  unsigned long tstart = millis();
  while (millis() - tstart < 15000) {
    while (Serial.available()) {
      char c = Serial.read();
      json += c;
      tstart = millis();
    }
    if (json.indexOf("\nEND") >= 0 || json.indexOf("\r\nEND") >= 0) break;
    delay(5);
  }

  // Strip END marker
  int endIdx = json.indexOf("\nEND");
  if (endIdx < 0) endIdx = json.indexOf("\r\nEND");
  if (endIdx >= 0) json = json.substring(0, endIdx);
  json.trim();

  // Parse
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("ERR:JSON_PARSE");
    return;
  }

  // Read existing config from SPIFFS
  DynamicJsonDocument cfg(512);
  if (SPIFFS.exists(JSON_CONFIG_FILE)) {
    File f = SPIFFS.open(JSON_CONFIG_FILE, "r");
    deserializeJson(cfg, f);
    f.close();
  }

  // Apply fields from browser
  // Theme Builder sends: wifi_ssid, wifi_pass, url, port, wallet, pass2
  if (doc.containsKey("wifi_ssid")) cfg[JSON_KEY_SSID]            = doc["wifi_ssid"].as<String>();
  if (doc.containsKey("wifi_pass")) cfg[JSON_KEY_PASW]             = doc["wifi_pass"].as<String>();
  if (doc.containsKey("url"))       cfg[JSON_SPIFFS_KEY_POOLURL]   = doc["url"].as<String>();
  if (doc.containsKey("port"))      cfg[JSON_SPIFFS_KEY_POOLPORT]  = doc["port"].as<int>();
  if (doc.containsKey("wallet"))    cfg[JSON_SPIFFS_KEY_WALLETID]  = doc["wallet"].as<String>();
  if (doc.containsKey("pass2"))     cfg[JSON_SPIFFS_KEY_POOLPASS]  = doc["pass2"].as<String>();

  // Write back to SPIFFS
  File f = SPIFFS.open(JSON_CONFIG_FILE, "w");
  serializeJson(cfg, f);
  f.close();

  Serial.println("OK");
  Serial.flush();
  delay(200);
  ESP.restart();
}
