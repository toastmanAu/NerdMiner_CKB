#include "telegramOTA.h"
#ifdef TELEGRAM_OTA

#include <Arduino.h>
#include <FastBot.h>

// ── Compile-time guards ────────────────────────────────────────────────────
#ifndef TG_BOT_TOKEN
  #error "TELEGRAM_OTA: define TG_BOT_TOKEN in build_flags, e.g. -D TG_BOT_TOKEN=\"123:ABC\""
#endif
#ifndef TG_ALLOWED_CHAT_ID
  #error "TELEGRAM_OTA: define TG_ALLOWED_CHAT_ID in build_flags, e.g. -D TG_ALLOWED_CHAT_ID=123456789"
#endif

// Stringify helpers
#define _STR(x) #x
#define XSTR(x) _STR(x)

// ── Globals ────────────────────────────────────────────────────────────────
static FastBot bot(TG_BOT_TOKEN);
static const char* TAG = "[TelegramOTA]";

// ── Message handler ────────────────────────────────────────────────────────
static void onMessage(FB_msg& msg) {
    // Security: ignore anyone who isn't the allowed chat
    // Arduino String has no toInt64(); compare as strings instead.
    if (msg.chatID != XSTR(TG_ALLOWED_CHAT_ID)) {
        Serial.printf("%s Ignored message from unauthorised chat: %s\n",
                      TAG, msg.chatID.c_str());
        return;
    }

    if (msg.OTA) {
        // .bin file received — initiate OTA flash
        Serial.printf("%s OTA file received, flashing...\n", TAG);
        bot.sendMessage("⚡ NerdMiner CKB: OTA update received, flashing...", msg.chatID);
        delay(100); // let the message send before we block
        if (bot.updateFS()) {
            // updateFS() reboots on success — won't reach here
            bot.sendMessage("✅ Flash complete, rebooting!", msg.chatID);
        } else {
            Serial.printf("%s OTA flash FAILED\n", TAG);
            bot.sendMessage("❌ OTA flash failed. Check serial for details.", msg.chatID);
        }
    } else if (msg.text == "/status") {
        String reply = "⛏ NerdMiner CKB online\n";
        reply += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
        reply += "Uptime: " + String(millis() / 1000) + "s\n";
        reply += "Send a .bin file to flash new firmware.";
        bot.sendMessage(reply, msg.chatID);
    }
    // Silently ignore everything else
}

// ── FreeRTOS task ──────────────────────────────────────────────────────────
static void telegramOtaTask(void* /*param*/) {
    Serial.printf("%s Task started. Allowed chat ID: %s\n",
                  TAG, XSTR(TG_ALLOWED_CHAT_ID));
    bot.attach(onMessage);
    bot.setChatID(XSTR(TG_ALLOWED_CHAT_ID));  // whitelist — ignore all others
    bot.setPeriod(3000);                        // poll every 3 s

    // Announce we're alive (optional — comment out if you don't want this)
    bot.sendMessage("🟢 NerdMiner CKB online. Send /status or a .bin to OTA.",
                    XSTR(TG_ALLOWED_CHAT_ID));

    while (true) {
        bot.tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── Public init ───────────────────────────────────────────────────────────
void initTelegramOTA() {
    // Stack: 8KB is enough for FastBot + HTTP client
    xTaskCreatePinnedToCore(
        telegramOtaTask,
        "TelegramOTA",
        8192,
        nullptr,
        1,           // low priority — mining takes precedence
        nullptr,
        1            // core 1 alongside monitor/stratum
    );
}

#endif // TELEGRAM_OTA
