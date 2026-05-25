// ClawVoice — Voice-first OpenClaw Cardputer Client
// State machine: SLEEP → (Fn press) → RECORDING → PROCESSING → SPEAKING → SLEEP
// Transport: USB serial (priority) with WiFi fallback, both maintained.
// Storage: SD card (Cardputer ADV) with DRAM fallback.

#include <M5Cardputer.h>
#include <WiFi.h>
#include "config.h"
#include "audio.h"
#include "network.h"
#include "settings.h"

// ── Globals ─────────────────────────────────────────────────
Audio   audio;
Network network;

// Status callback — bridges network.h → audio.h
void onNetworkStatus(const char* text) {
    audio.showStatus(text);
}
State   currentState   = State::SLEEP;
unsigned long stateEnterTime = 0;
unsigned long lastActivity   = 0;

// ── WiFi background maintenance ─────────────────────────────
unsigned long lastWifiAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000;  // retry every 10s
bool wifiConnected = false;
WifiConfig wifiCfg;  // loaded from SD at startup

void maintainWifi() {
    wl_status_t status = WiFi.status();
    wifiConnected = (status == WL_CONNECTED);

    if (wifiConnected) return;  // all good

    if (millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL) return;
    lastWifiAttempt = millis();

    Serial.printf("[WIFI] status=%d, reconnecting...\n", (int)status);
    if (status == WL_DISCONNECTED || status == WL_CONNECTION_LOST) {
        WiFi.reconnect();
    } else if (status == WL_NO_SHIELD || status == WL_CONNECT_FAILED) {
        WiFi.disconnect(false, true);  // full reset
        delay(100);
        WiFi.begin(wifiCfg.ssid, wifiCfg.pass);
    }
}

// ── USB bridge detection ────────────────────────────────────
unsigned long lastUsbPoll = 0;
const unsigned long USB_POLL_INTERVAL = 3000;  // poll every 3s

void maintainUsb() {
    if (millis() - lastUsbPoll < USB_POLL_INTERVAL) return;
    lastUsbPoll = millis();

    if (network.usbPoll()) {
        network.transport = Transport::USB;
        Serial.println("[MAIN] Switched to USB");
    }
}

// ── Combined transport priority ─────────────────────────────
// USB > WiFi. Returns the active transport.
Transport activeTransport() {
    if (network.transport == Transport::USB) {
        // TODO: detect USB disconnect, fallback to WiFi
        return Transport::USB;
    }
    return Transport::WIFI;
}

// ── Fn Key ──────────────────────────────────────────────────
bool fnDown = false;

void updateFnState() {
    fnDown = M5Cardputer.Keyboard.keysState().fn;
}

bool fnIsHeld() {
    return fnDown;
}

// ── Helpers ─────────────────────────────────────────────────
void enterState(State s) {
    currentState = s;
    stateEnterTime = millis();
    if (s != State::SLEEP) lastActivity = millis();
}

// ── State Handlers ──────────────────────────────────────────

void handleSleep() {
    audio.drawState(State::SLEEP);
}

void handleRecording() {
    audio.drawState(State::RECORDING);

    // Flush mic data to SD card in chunks
    if (audio.useSD) {
        audio.micTick();
    }

    bool tooLong = audio.recordDuration() >= audio.getMaxRecordSec();
    bool fnReleased = !fnIsHeld();

    if (fnReleased || tooLong) {
        float dur = audio.recordDuration();
        audio.micStop();
        Serial.printf("[MAIN] Stopped: %.2fs, %zu samples\n",
                      dur, audio.recordedSamples);

        if (audio.recordedSamples == 0) {
            Serial.println("[MAIN] No samples, discarding");
            enterState(State::SLEEP);
            return;
        }

        enterState(State::PROCESSING);
    }
}

void handleProcessing() {
    audio.drawState(State::PROCESSING);

    size_t responseSamples = 0;
    Transport t = activeTransport();

    if (t == Transport::USB) {
        // ── USB mode ───────────────────────────────────
        size_t recBytes = 0;
        int16_t* recData = audio.getRecordData(recBytes);
        if (recData) {
            responseSamples = network.sendVoiceUSB(
                recData, recBytes / sizeof(int16_t),
                audio.useSD, 0,
                [](){ M5Cardputer.update(); audio.drawState(State::PROCESSING); }
            );
        } else {
            responseSamples = network.sendVoiceUSB(
                nullptr, 0,
                audio.useSD, recBytes,
                [](){ M5Cardputer.update(); audio.drawState(State::PROCESSING); }
            );
        }
    } else {
        // ── WiFi mode ──────────────────────────────────
        if (!wifiConnected) {
            Serial.println("[MAIN] No WiFi, can't send");
            enterState(State::ERROR_WAIT);
            return;
        }

        if (audio.useSD) {
            size_t recBytes = 0;
            int16_t* recData = audio.getRecordData(recBytes);
            if (recData) {
                responseSamples = network.sendVoice(
                    recData, recBytes / sizeof(int16_t),
                    nullptr, 0, true, 0,
                    [](){ M5Cardputer.update(); audio.drawState(State::PROCESSING); }
                );
            } else {
                responseSamples = network.sendVoice(
                    nullptr, 0, nullptr, 0, true, recBytes,
                    [](){ M5Cardputer.update(); audio.drawState(State::PROCESSING); }
                );
            }
        } else {
            responseSamples = network.sendVoice(
                audio.buffer, audio.recordedSamples,
                audio.buffer, audio.bufSamples, false, 0,
                [](){ M5Cardputer.update(); audio.drawState(State::PROCESSING); }
            );
        }
    }

    if (responseSamples == 0) {
        Serial.println("[MAIN] Pipeline failed");
        enterState(State::ERROR_WAIT);
        return;
    }

    audio.speakerStart();

    if (audio.useSD) {
        audio.playSD(SD_TTS_FILE);  // start non-blocking
    } else {
        audio.playPCM(responseSamples);
    }
    enterState(State::SPEAKING);
}

void handleSpeaking() {
    audio.drawState(State::SPEAKING);

    bool more;
    if (audio.useSD) {
        more = audio.playChunk();
    } else {
        more = audio.isPlaying();
    }

    if (!more) {
        if (audio.useSD || !audio.isPlaying()) {
            audio.stopPlayback();
            audio.cycleCount++;
            enterState(State::SLEEP);
        }
        return;
    }

    if (fnIsHeld()) {
        audio.stopPlayback();
        enterState(State::SLEEP);
    }
}

void handleErrorWait() {
    audio.drawState(State::ERROR_WAIT);
    if (millis() - stateEnterTime > ERROR_DISPLAY_MS) {
        enterState(State::SLEEP);
    }
}

// ── Setup ───────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.setRxBufferSize(4096);  // bigger USB RX buffer for TTS streaming
    delay(200);
    Serial.println("\n\n=== ClawVoice ===");
    Serial.flush();
    delay(100);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_WHITE);

    if (!audio.begin()) {
        Serial.println("[FATAL] Audio buffer alloc failed!");
        M5Cardputer.Display.drawString("NO RAM!", 10, 10);
        while (1) delay(1000);
    }

    // ── Load WiFi config from SD ─────────────────────────
    if (audio.sdAvailable) {
        loadWifiConfig(wifiCfg);
    }

    // ── Setup page trigger (hold Fn at startup) ────────────
    delay(500);  // wait for keyboard init
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.keysState().fn) {
        Serial.println("[MAIN] Fn held at startup — entering WiFi setup");
        if (audio.sdAvailable) {
            runSetupPage(wifiCfg);  // returns after save/cancel
        } else {
            M5Cardputer.Display.fillScreen(TFT_BLACK);
            M5Cardputer.Display.setTextColor(TFT_RED);
            M5Cardputer.Display.drawString("Need SD card!", 4, 60);
            M5Cardputer.Display.drawString("for WiFi setup", 4, 74);
            delay(2000);
        }
    }

    // ── Start both transports ────────────────────────────
    network.begin();          // init proxyHost/proxyPort (ESP32 global ctor workaround)
    network.transport = Transport::WIFI;
    network.onStatus = onNetworkStatus;  // wire up status display
    WiFi.mode(WIFI_STA);  // Explicit station mode
    WiFi.setAutoReconnect(true);
    Serial.printf("[WIFI] Connecting to %s (background)\n", wifiCfg.ssid);
    WiFi.begin(wifiCfg.ssid, wifiCfg.pass);

    M5Cardputer.Speaker.setVolume(255);
    // Apply volume from config
    audio.applyVolume(wifiCfg.volume);
    enterState(State::SLEEP);
    lastActivity = millis();
}

// ── Loop ────────────────────────────────────────────────────
void loop() {
    M5Cardputer.update();
    updateFnState();

    // ── Background transport maintenance (always) ────────
    maintainWifi();
    if (currentState == State::SLEEP) {
        maintainUsb();
    }

    if (currentState == State::SLEEP) {
        // ── Check for Tab key → WiFi setup ────────────
        auto& ks = M5Cardputer.Keyboard.keysState();
        if (ks.tab) {
            Serial.println("[MAIN] Tab pressed — entering WiFi setup");
            if (audio.sdAvailable) {
                runSetupPage(wifiCfg);
                // Force full screen redraw on return
                audio.lastDrawnState = State::AWAKE;
                audio.statusText[0] = '\0';
                enterState(State::SLEEP);
            } else {
                M5Cardputer.Display.fillScreen(TFT_BLACK);
                M5Cardputer.Display.setTextColor(TFT_RED);
                M5Cardputer.Display.drawString("Need SD card!", 4, 60);
                delay(1500);
            }
            return;
        }

        if (fnIsHeld()) {
            Serial.println("[MAIN] Recording...");
            audio.recordedSamples = 0;
            audio.micStart();
            enterState(State::RECORDING);
        }
        handleSleep();
    } else {
        switch (currentState) {
            case State::RECORDING:  handleRecording();  break;
            case State::PROCESSING: handleProcessing(); break;
            case State::SPEAKING:   handleSpeaking();   break;
            case State::ERROR_WAIT: handleErrorWait();  break;
            default: enterState(State::SLEEP); break;
        }

        if (currentState != State::SLEEP &&
            millis() - lastActivity > SLEEP_AFTER_MS) {
            if (currentState == State::SPEAKING) audio.stopPlayback();
            enterState(State::SLEEP);
        }
    }

    delay(33);
}
