#pragma once
// ClawVoice — Settings: SD card config persistence + on-screen setup page
// Stores WiFi SSID/password in /wifi.cfg on SD card.
// Provides a keyboard-driven setup page for entering WiFi credentials.

#include <M5Cardputer.h>
#include <SD.h>
#include "config.h"

// ── Config file format ──────────────────────────────────────
// /wifi.cfg — two lines:
//   line 1: SSID
//   line 2: password
// Both are plain text, max 64 chars each, no trailing newlines in stored values.

static constexpr const char* WIFI_CFG_FILE = "/wifi.cfg";

// Runtime config values (populated from SD or compile-time defaults)
struct WifiConfig {
    char ssid[65] = {0};
    char pass[65] = {0};
};

// ── Load WiFi config from SD ────────────────────────────────
// Returns true if /wifi.cfg was found and parsed.
// Falls back to compile-time defaults if file doesn't exist.
static bool loadWifiConfig(WifiConfig& cfg) {
    // Start with compile-time defaults
    strncpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, WIFI_PASS, sizeof(cfg.pass) - 1);

    File f = SD.open(WIFI_CFG_FILE, FILE_READ);
    if (!f) {
        Serial.println("[CFG] No /wifi.cfg, using compile-time defaults");
        return false;
    }

    // Read SSID line
    int idx = 0;
    while (f.available() && idx < (int)sizeof(cfg.ssid) - 1) {
        char c = f.read();
        if (c == '\n' || c == '\r') {
            if (c == '\r') {
                if (f.available() && f.peek() == '\n') f.read();
            }
            break;
        }
        cfg.ssid[idx++] = c;
    }
    cfg.ssid[idx] = '\0';

    // Read password line
    idx = 0;
    while (f.available() && idx < (int)sizeof(cfg.pass) - 1) {
        char c = f.read();
        if (c == '\n' || c == '\r') break;
        cfg.pass[idx++] = c;
    }
    cfg.pass[idx] = '\0';

    f.close();
    Serial.printf("[CFG] Loaded WiFi: SSID=\"%s\" pass=%d chars\n", cfg.ssid, strlen(cfg.pass));
    return true;
}

// ── Save WiFi config to SD ──────────────────────────────────
static bool saveWifiConfig(const WifiConfig& cfg) {
    SD.remove(WIFI_CFG_FILE);
    File f = SD.open(WIFI_CFG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[CFG] Can't write /wifi.cfg");
        return false;
    }
    f.printf("%s\n%s\n", cfg.ssid, cfg.pass);
    f.close();
    Serial.printf("[CFG] Saved WiFi: SSID=\"%s\"\n", cfg.ssid);
    return true;
}

// ── Setup Page ──────────────────────────────────────────────

enum class SetupField { SSID, PASSWORD, SAVE, CANCEL };

static constexpr int FIELD_SSID     = 0;
static constexpr int FIELD_PASSWORD = 1;
static constexpr int FIELD_SAVE     = 2;
static constexpr int FIELD_CANCEL   = 3;
static constexpr int FIELD_COUNT    = 4;

// Draw the setup page
static void drawSetupPage(const WifiConfig& cfg, SetupField field, unsigned long tickMs) {
    auto& dsp = M5Cardputer.Display;
    int w = dsp.width();
    int h = dsp.height();

    dsp.fillScreen(TFT_BLACK);
    dsp.setTextSize(1);
    dsp.setTextColor(TFT_WHITE);
    dsp.drawString("WiFi Setup", (w - 10 * 6) / 2, 2);

    int y = 20;
    int fieldIdx = (int)field;

    // SSID field
    {
        int x = 4;
        dsp.setTextColor(fieldIdx == FIELD_SSID ? TFT_YELLOW : TFT_CYAN);
        dsp.drawString("SSID:", x, y);
        x += 5 * 6 + 4;

        if (fieldIdx == FIELD_SSID) {
            dsp.fillRect(x - 2, y - 1, w - x + 2, 10, TFT_NAVY);
        }
        dsp.setTextColor(TFT_WHITE);
        char display[65];
        strncpy(display, cfg.ssid, sizeof(display) - 1);
        dsp.drawString(display, x, y);

        // Cursor blink
        if (fieldIdx == FIELD_SSID && (tickMs / 500) % 2 == 0) {
            int cx = x + strlen(display) * 6;
            dsp.drawFastVLine(cx, y, 8, TFT_WHITE);
        }
    }
    y += 14;

    // Password field
    {
        int x = 4;
        dsp.setTextColor(fieldIdx == FIELD_PASSWORD ? TFT_YELLOW : TFT_CYAN);
        dsp.drawString("PASS:", x, y);
        x += 5 * 6 + 4;

        if (fieldIdx == FIELD_PASSWORD) {
            dsp.fillRect(x - 2, y - 1, w - x + 2, 10, TFT_NAVY);
        }
        dsp.setTextColor(TFT_WHITE);
        char display[65];
        int plen = strlen(cfg.pass);
        for (int i = 0; i < plen && i < 64; i++) display[i] = '*';
        display[plen > 64 ? 64 : plen] = '\0';
        dsp.drawString(display, x, y);

        if (fieldIdx == FIELD_PASSWORD && (tickMs / 500) % 2 == 0) {
            int cx = x + plen * 6;
            dsp.drawFastVLine(cx, y, 8, TFT_WHITE);
        }
    }
    y += 16;

    // Save button
    {
        int x = 4;
        if (fieldIdx == FIELD_SAVE) {
            dsp.fillRoundRect(x, y - 1, w - 8, 12, 3, TFT_BLUE);
            dsp.setTextColor(TFT_WHITE);
        } else {
            dsp.setTextColor(TFT_GREEN);
        }
        dsp.drawString("[ Save & Exit ]", x + 4, y);
    }
    y += 16;

    // Cancel button
    {
        int x = 4;
        if (fieldIdx == FIELD_CANCEL) {
            dsp.fillRoundRect(x, y - 1, w - 8, 12, 3, TFT_BLUE);
            dsp.setTextColor(TFT_WHITE);
        } else {
            dsp.setTextColor(TFT_RED);
        }
        dsp.drawString("[ Cancel ]", x + 4, y);
    }

    // Help text
    dsp.setTextColor(0x8410);
    dsp.drawString("Tab/Down: next  Bksp: del", 4, h - 12);
    dsp.drawString("Up: prev  Home: reboot", 4, h - 4);
}

// Run the WiFi setup page. Blocks until user saves or cancels.
// Returns true if config was saved (caller should reconnect WiFi).
static bool runSetupPage(WifiConfig& cfg) {
    auto& dsp = M5Cardputer.Display;
    auto& kbd = M5Cardputer.Keyboard;

    SetupField field = SetupField::SSID;
    unsigned long startMs = millis();
    bool redraw = true;
    bool confChanged = false;

    // Track last key state to detect new presses
    auto lastKeysState = kbd.keysState();
    int lastKeysSize = -1;

    while (true) {
        M5Cardputer.update();

        // ── Detect new key presses ────────────────────────
        // The word buffer grows when new printable keys are pressed
        kbd.updateKeysState();
        auto& ks = kbd.keysState();
        bool newPress = false;

        // Check for new key events: word vector has new chars
        static std::vector<char> prevWord;
        if (ks.word.size() > prevWord.size()) {
            newPress = true;
        }

        // Check edge on special keys that don't add to word
        bool enterNow = ks.enter && !lastKeysState.enter;
        bool delNow   = ks.del && !lastKeysState.del;
        bool tabNow   = ks.tab && !lastKeysState.tab;

        // ── Handle Enter ──────────────────────────────────
        if (enterNow) {
            if (field == SetupField::SSID) {
                field = SetupField::PASSWORD;
                prevWord.clear();
            } else if (field == SetupField::PASSWORD) {
                field = SetupField::SAVE;
                prevWord.clear();
            } else if (field == SetupField::SAVE) {
                // Save and return (setup() continues with new creds)
                Serial.println("[SETUP] Saving config...");
                saveWifiConfig(cfg);
                confChanged = true;
                break;
            } else if (field == SetupField::CANCEL) {
                confChanged = false;
                break;
            }
            prevWord.clear();
            redraw = true;
        }

        // ── Handle Tab / Down arrow → next field ──────────
        if (tabNow || (enterNow && field == SetupField::SAVE)) {
            // already handled above
        }
        if (tabNow) {
            int next = ((int)field + 1) % FIELD_COUNT;
            field = (SetupField)next;
            prevWord.clear();
            redraw = true;
        }

        // ── Handle Up arrow / Shift+Tab → prev field ──────
        // Check if Ctrl is held: use arrows
        if (ks.ctrl && ks.word.size() > prevWord.size()) {
            // ctrl+letter won't produce chars, ignore
        }

        // ── Handle Backspace ──────────────────────────────
        if (delNow) {
            if (field == SetupField::SSID) {
                int len = strlen(cfg.ssid);
                if (len > 0) cfg.ssid[len - 1] = '\0';
            } else if (field == SetupField::PASSWORD) {
                int len = strlen(cfg.pass);
                if (len > 0) cfg.pass[len - 1] = '\0';
            }
            prevWord.clear();
            redraw = true;
        }

        // ── Handle printable characters ───────────────────
        if (newPress && ks.word.size() > 0) {
            char c = ks.word.back();  // most recently typed char
            if (field == SetupField::SSID) {
                int len = strlen(cfg.ssid);
                if (len < (int)sizeof(cfg.ssid) - 1) {
                    cfg.ssid[len] = c;
                    cfg.ssid[len + 1] = '\0';
                }
            } else if (field == SetupField::PASSWORD) {
                int len = strlen(cfg.pass);
                if (len < (int)sizeof(cfg.pass) - 1) {
                    cfg.pass[len] = c;
                    cfg.pass[len + 1] = '\0';
                }
            }
            redraw = true;
        }

        // ── Handle modifier for field navigation ──────────
        // Fn + U = up, Fn + J = down (Cardputer keyboard nav convention)
        if (ks.fn) {
            if (newPress && ks.word.size() > 0) {
                char nav = ks.word.back();
                if (nav == 'u' || nav == 'U') {
                    int prev = (int)field - 1;
                    if (prev < 0) prev = FIELD_COUNT - 1;
                    field = (SetupField)prev;
                    prevWord.clear();
                    redraw = true;
                } else if (nav == 'j' || nav == 'J') {
                    int next = ((int)field + 1) % FIELD_COUNT;
                    field = (SetupField)next;
                    prevWord.clear();
                    redraw = true;
                }
            }
        }

        // ── Save previous state ───────────────────────────
        lastKeysState = ks;
        prevWord = ks.word;

        // ── Redraw if needed ──────────────────────────────
        if (redraw) {
            drawSetupPage(cfg, field, millis() - startMs);
            redraw = false;
        }

        // ── Redraw cursor blink periodically ──────────────
        unsigned long elapsed = millis() - startMs;
        if (elapsed > 30 && (elapsed % 250 < 5)) {
            drawSetupPage(cfg, field, millis() - startMs);
        }

        delay(20);
    }

    return confChanged;
}
