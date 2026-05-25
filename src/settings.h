#pragma once
// ClawVoice — Settings: SD card config persistence + on-screen setup page
// Stores WiFi SSID/password in /wifi.cfg on SD card.
// Provides a keyboard-driven setup page for entering WiFi credentials.

#include <M5Cardputer.h>
#include <SD.h>
#include "config.h"

// ── Config file format ──────────────────────────────────────
// /wifi.cfg — three lines:
//   line 1: SSID
//   line 2: password
//   line 3: volume (0-255)
// SSID/pass are plain text, max 64 chars each, no trailing newlines.
// Volume is a decimal number.

static constexpr const char* WIFI_CFG_FILE = "/wifi.cfg";

// Runtime config values (populated from SD or compile-time defaults)
struct WifiConfig {
    char ssid[65] = {0};
    char pass[65] = {0};
    int  volume   = 255;  // 0-255 speaker volume
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

    // Read volume line
    char volBuf[8] = {0};
    idx = 0;
    while (f.available() && idx < 7) {
        char c = f.read();
        if (c == '\n' || c == '\r') break;
        volBuf[idx++] = c;
    }
    volBuf[idx] = '\0';
    if (idx > 0) {
        int v = atoi(volBuf);
        if (v >= 0 && v <= 255) cfg.volume = v;
    }

    f.close();
    Serial.printf("[CFG] Loaded WiFi: SSID=\"%s\" pass=%d chars vol=%d\n",
                  cfg.ssid, strlen(cfg.pass), cfg.volume);
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
    f.printf("%s\n%s\n%d\n", cfg.ssid, cfg.pass, cfg.volume);
    f.close();
    Serial.printf("[CFG] Saved WiFi: SSID=\"%s\" vol=%d\n", cfg.ssid, cfg.volume);
    return true;
}

// ── Setup Page ──────────────────────────────────────────────

enum class SetupField { SSID, PASSWORD, VOLUME, SAVE, CANCEL };

static constexpr int FIELD_SSID     = 0;
static constexpr int FIELD_PASSWORD = 1;
static constexpr int FIELD_VOLUME   = 2;
static constexpr int FIELD_SAVE     = 3;
static constexpr int FIELD_CANCEL   = 4;
static constexpr int FIELD_COUNT    = 5;

// Draw the setup page
static void drawSetupPage(const WifiConfig& cfg, SetupField field, unsigned long tickMs) {
    auto& dsp = M5Cardputer.Display;
    int w = dsp.width();
    int h = dsp.height();

    dsp.fillScreen(TFT_BLACK);
    dsp.setTextSize(1);
    dsp.setTextColor(TFT_WHITE);
    dsp.drawString("Settings", (w - 8 * 6) / 2, 2);

    int y = 18;
    int fieldIdx = (int)field;

    // SSID field
    {
        int x = 4;
        dsp.setTextColor(fieldIdx == FIELD_SSID ? TFT_YELLOW : TFT_CYAN);
        dsp.drawString("WiFi:", x, y);
        x += 5 * 6 + 4;

        if (fieldIdx == FIELD_SSID) {
            dsp.fillRect(x - 2, y - 1, w - x + 2, 10, TFT_NAVY);
        }
        dsp.setTextColor(TFT_WHITE);
        char display[65];
        strncpy(display, cfg.ssid, sizeof(display) - 1);
        dsp.drawString(display, x, y);

        if (fieldIdx == FIELD_SSID && (tickMs / 500) % 2 == 0) {
            int cx = x + strlen(display) * 6;
            dsp.drawFastVLine(cx, y, 8, TFT_WHITE);
        }
    }
    y += 12;

    // Password field
    {
        int x = 4;
        dsp.setTextColor(fieldIdx == FIELD_PASSWORD ? TFT_YELLOW : TFT_CYAN);
        dsp.drawString("Pass:", x, y);
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
    y += 12;

    // Volume field
    {
        int x = 4;
        dsp.setTextColor(fieldIdx == FIELD_VOLUME ? TFT_YELLOW : TFT_CYAN);
        dsp.drawString("Vol:", x, y);
        x += 4 * 6 + 4;

        if (fieldIdx == FIELD_VOLUME) {
            dsp.fillRect(x - 2, y - 1, w - x + 2, 10, TFT_NAVY);
        }
        dsp.setTextColor(TFT_WHITE);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", cfg.volume);
        dsp.drawString(buf, x, y);

        // Volume bar (0-255 → 0-128px)
        int barW = (cfg.volume * (w - x - 30)) / 255;
        int barX = x + 30;
        dsp.drawRect(barX, y, w - barX - 4, 8, 0x8410);
        if (barW > 0) {
            dsp.fillRect(barX + 1, y + 1, barW, 6,
                         cfg.volume > 200 ? TFT_RED : (cfg.volume > 100 ? TFT_YELLOW : TFT_GREEN));
        }

        if (fieldIdx == FIELD_VOLUME && (tickMs / 500) % 2 == 0) {
            dsp.drawFastVLine(x + strlen(buf) * 6, y, 8, TFT_WHITE);
        }
    }
    y += 14;

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
    y += 15;

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
    dsp.drawString("Fn+J/Down:next  Fn+U/Up:prev", 4, h - 12);
    dsp.drawString("Enter:select  Bksp:del", 4, h - 4);
}

// Run the WiFi setup page. Blocks until user saves or cancels.
// Run the WiFi setup page. Blocks until user saves or cancels.
static bool runSetupPage(WifiConfig& cfg) {
    auto& dsp = M5Cardputer.Display;
    auto& kbd = M5Cardputer.Keyboard;

    SetupField field = SetupField::SSID;
    unsigned long startMs = millis();
    bool redraw = true;
    bool confChanged = false;

    auto lastKeysState = kbd.keysState();
    std::vector<char> prevWord;

    while (true) {
        M5Cardputer.update();
        kbd.updateKeysState();
        auto& ks = kbd.keysState();
        bool newPress = false;

        if (ks.word.size() > prevWord.size()) {
            newPress = true;
        }

        bool fnNow  = ks.fn;
        bool enterNow = ks.enter && !lastKeysState.enter;
        bool delNow   = ks.del && !lastKeysState.del;
        bool tabNow   = ks.tab && !lastKeysState.tab;

        // ── Enter: select/confirm current field ──────────
        if (enterNow) {
            if (field == SetupField::SSID) {
                field = SetupField::PASSWORD;
            } else if (field == SetupField::PASSWORD) {
                field = SetupField::VOLUME;
            } else if (field == SetupField::VOLUME) {
                field = SetupField::SAVE;
            } else if (field == SetupField::SAVE) {
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

        // ── Tab: next field ─────────────────────────────
        if (tabNow) {
            int next = ((int)field + 1) % FIELD_COUNT;
            field = (SetupField)next;
            prevWord.clear();
            redraw = true;
        }

        // ── Fn navigation: Fn+J=down, Fn+U=up ──────────
        if (fnNow && newPress && ks.word.size() > 0) {
            char nav = ks.word.back();
            if (nav == 'j' || nav == 'J') {
                int next = ((int)field + 1) % FIELD_COUNT;
                field = (SetupField)next;
                prevWord.clear();
                redraw = true;
            } else if (nav == 'u' || nav == 'U') {
                int prev = (int)field - 1;
                if (prev < 0) prev = FIELD_COUNT - 1;
                field = (SetupField)prev;
                prevWord.clear();
                redraw = true;
            } else if (nav == 'l' || nav == 'L') {
                // Decrease volume
                if (field == SetupField::VOLUME && cfg.volume >= 16) {
                    cfg.volume -= 16;
                    redraw = true;
                }
            } else if (nav == 'p' || nav == 'P') {
                // Increase volume
                if (field == SetupField::VOLUME && cfg.volume <= 239) {
                    cfg.volume += 16;
                    redraw = true;
                }
            }
        }

        // ── Backspace: delete char (SSID/Pass) or ↓ vol ─
        if (delNow) {
            if (field == SetupField::SSID) {
                int len = strlen(cfg.ssid);
                if (len > 0) cfg.ssid[len - 1] = '\0';
            } else if (field == SetupField::PASSWORD) {
                int len = strlen(cfg.pass);
                if (len > 0) cfg.pass[len - 1] = '\0';
            } else if (field == SetupField::VOLUME) {
                if (cfg.volume >= 16) cfg.volume -= 16;
            }
            prevWord.clear();
            redraw = true;
        }

        // ── Printable chars (SSID/Pass only) ────────────
        if (newPress && ks.word.size() > 0 && !fnNow) {
            char c = ks.word.back();
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
            } else if (field == SetupField::VOLUME && c >= '0' && c <= '9') {
                // Digit input for volume
                int v = cfg.volume * 10 + (c - '0');
                if (v <= 255) cfg.volume = v;
            }
            redraw = true;
        }

        // ── Space: toggle volume or add space to text ───
        if (ks.space && !lastKeysState.space) {
            if (field == SetupField::VOLUME) {
                // Quick toggle: low/med/high
                if (cfg.volume < 85) cfg.volume = 170;
                else if (cfg.volume < 200) cfg.volume = 255;
                else cfg.volume = 42;
                redraw = true;
            } else if (field == SetupField::SSID) {
                int len = strlen(cfg.ssid);
                if (len < (int)sizeof(cfg.ssid) - 1) {
                    cfg.ssid[len] = ' ';
                    cfg.ssid[len + 1] = '\0';
                }
            }
        }

        // ── Save previous state ──────────────────────────
        lastKeysState = ks;
        prevWord = ks.word;

        // ── Redraw ──────────────────────────────────────
        if (redraw) {
            drawSetupPage(cfg, field, millis() - startMs);
            redraw = false;
        }

        unsigned long elapsed = millis() - startMs;
        if (elapsed > 30 && (elapsed % 250 < 5)) {
            drawSetupPage(cfg, field, millis() - startMs);
        }

        delay(20);
    }

    return confChanged;
}
