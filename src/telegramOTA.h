#pragma once
#ifdef TELEGRAM_OTA

/**
 * telegramOTA.h — Telegram OTA firmware update for NerdMiner CKB
 *
 * Security model:
 *  - Only the chat ID stored in TG_ALLOWED_CHAT_ID can trigger an OTA.
 *  - All other senders are silently ignored.
 *  - Bot token and chat ID are set at compile time via build flags:
 *      -D TG_BOT_TOKEN=\"<token>\"
 *      -D TG_ALLOWED_CHAT_ID=<chat_id>   (numeric, no quotes)
 *
 * Usage:
 *  1. Add build flags above to your env in platformio.ini
 *  2. Add FastBot to lib_deps: GyverLibs/FastBot
 *  3. initTelegramOTA() is called from setup() after WiFi is connected.
 *  4. Send a compiled .bin file to the bot → board flashes and reboots.
 */

void initTelegramOTA();

#endif // TELEGRAM_OTA
