#pragma once
// ClawVoice — Audio: microphone recording, speaker playback
// Supports SD card (Cardputer ADV) with DRAM fallback.
//
// SD recording strategy:
//   Mic fills DRAM buffer (4s @ 16kHz = 128KB).
//   When buffer is full, write to SD, restart recording.
//   ~5ms gap per 4s chunk — negligible for speech.

#include <M5Cardputer.h>
#include "config.h"
#include "network.h"
#include <SPI.h>
#include <SD.h>

extern Network network;  // defined in main.cpp

class Audio {
public:
    // ── Storage mode ──────────────────────────────────────
    bool sdAvailable = false;
    bool useSD       = false;     // using SD for current operation

    // ── DRAM buffer ───────────────────────────────────────
    static constexpr size_t DRAM_BYTES = (size_t)(MIC_SAMPLE_RATE * MAX_RECORD_SEC_RAM) * sizeof(int16_t);
    int16_t* buffer     = nullptr;
    size_t   bufSamples = 0;

    // ── Recording state ───────────────────────────────────
    size_t   recordedSamples = 0;
    float    maxRecordSec    = MAX_RECORD_SEC_RAM;
    File     recFile;

    unsigned long recordStartMs = 0;

    // ── Playback state (non-blocking chunked from SD) ──────
    static constexpr size_t PLAY_CHUNK_SAMPLES = 2048;
    File   _playFile;
    size_t _playTotalSamples = 0;
    size_t _playPlayed       = 0;
    bool   _playingOpen      = false;

    // ── Init ──────────────────────────────────────────────
    bool begin() {
        buffer = (int16_t*)heap_caps_malloc(DRAM_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!buffer) {
            Serial.printf("[AUDIO] DRAM alloc failed! need=%zu heap=%u\n",
                          DRAM_BYTES, ESP.getFreeHeap());
            return false;
        }
        bufSamples = DRAM_BYTES / sizeof(int16_t);
        Serial.printf("[AUDIO] DRAM buffer: %zu bytes (%.1fs), heap=%u\n",
                      DRAM_BYTES, (float)bufSamples / MIC_SAMPLE_RATE, ESP.getFreeHeap());

        // Try SD card (Cardputer ADV: separate SPI bus from display)
        SPI.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_SPI_CS);
        if (SD.begin(SD_SPI_CS, SPI, 25000000)) {
            uint64_t totalBytes = SD.cardSize();
            Serial.printf("[AUDIO] SD OK: %.0f MB\n", totalBytes / 1048576.0);
            sdAvailable = true;
            maxRecordSec = MAX_RECORD_SEC_SD;
        } else {
            Serial.println("[AUDIO] SD not found — DRAM-only mode (4s limit)");
            sdAvailable = false;
            maxRecordSec = MAX_RECORD_SEC_RAM;
        }
        return true;
    }

    float getMaxRecordSec() const { return maxRecordSec; }

    // ── Microphone ────────────────────────────────────────

    void micStart() {
        M5Cardputer.Speaker.end();
        auto cfg = M5Cardputer.Mic.config();
        cfg.sample_rate      = MIC_SAMPLE_RATE;
        cfg.magnification    = 64;
        cfg.noise_filter_level = 64;
        cfg.task_priority    = 1;
        M5Cardputer.Mic.config(cfg);
        M5Cardputer.Mic.begin();
        recordedSamples = 0;
        recordStartMs = millis();

        useSD = sdAvailable;
        if (useSD) {
            SD.remove(SD_RECORD_FILE);
            recFile = SD.open(SD_RECORD_FILE, FILE_WRITE);
            if (!recFile) {
                Serial.println("[AUDIO] SD write failed, falling back to DRAM");
                useSD = false;
            }
        }

        M5Cardputer.Mic.record(buffer, bufSamples, MIC_SAMPLE_RATE);
    }

    // Call in recording loop — flushes full DRAM buffer to SD
    void micTick() {
        if (!useSD) return;

        // isRecording(): 0=done, 1=one buf pending, 2=both full
        // When 0 → buffer is fully filled → write to SD
        if (M5Cardputer.Mic.isRecording() == 0) {
            size_t bytes = bufSamples * sizeof(int16_t);
            recFile.write((const uint8_t*)buffer, bytes);
            recordedSamples += bufSamples;

            // Restart recording (unless max reached)
            if (recordDuration() < maxRecordSec) {
                M5Cardputer.Mic.record(buffer, bufSamples, MIC_SAMPLE_RATE);
            }
        }
    }

    void micStop() {
        if (useSD) {
            // Check if current buffer has data (partial or full)
            if (M5Cardputer.Mic.isRecording() != 0) {
                // Recording was interrupted mid-buffer.
                // Wait briefly for current chunk to finish (max 4s / already elapsed)
                // Actually, just use elapsed time to estimate partial samples
                float totalSec = (millis() - recordStartMs) / 1000.0f;
                size_t totalEst = (size_t)(totalSec * MIC_SAMPLE_RATE);
                // Subtract already-flushed samples
                size_t partialSamples = totalEst - recordedSamples;
                if (partialSamples > bufSamples) partialSamples = bufSamples;
                // Write partial buffer
                if (partialSamples > 0 && recFile) {
                    recFile.write((const uint8_t*)buffer, partialSamples * sizeof(int16_t));
                    recordedSamples += partialSamples;
                }
            }
            if (recFile) recFile.close();
        } else {
            float elapsed = (millis() - recordStartMs) / 1000.0f;
            recordedSamples = (size_t)(elapsed * MIC_SAMPLE_RATE);
            if (recordedSamples > bufSamples) recordedSamples = bufSamples;
        }

        M5Cardputer.Mic.end();
        M5Cardputer.Speaker.begin();
        M5Cardputer.Speaker.setVolume(volumeSetting);

        Serial.printf("[AUDIO] Recorded %.1fs (%zu samples) [%s]\n",
                      (float)recordedSamples / MIC_SAMPLE_RATE,
                      recordedSamples,
                      useSD ? "SD" : "DRAM");
    }

    float recordDuration() const {
        if (recordedSamples > 0) return (float)recordedSamples / MIC_SAMPLE_RATE;
        return (millis() - recordStartMs) / 1000.0f;
    }

    // ── Speaker ───────────────────────────────────────────

    void speakerStart() {
        if (!M5Cardputer.Speaker.isEnabled()) {
            M5Cardputer.Speaker.begin();
            M5Cardputer.Speaker.setVolume(volumeSetting);
        }
    }

    int volumeSetting = 255;  // current speaker volume (0-255)

    // Apply volume from config (0-255)
    void applyVolume(int vol) {
        if (vol < 0) vol = 0;
        if (vol > 255) vol = 255;
        volumeSetting = vol;
        M5Cardputer.Speaker.setVolume(vol);
        Serial.printf("[AUDIO] Volume set to %d\n", vol);
    }

    // Play from DRAM buffer (no SD mode)
    void playPCM(size_t pcmSamples) {
        if (pcmSamples == 0) return;
        Serial.printf("[AUDIO] playPCM: %zu samples (%.1fs @ %dHz) [DRAM]\n",
                      pcmSamples, (float)pcmSamples / PLAY_SAMPLE_RATE, PLAY_SAMPLE_RATE);
        M5Cardputer.Speaker.playRaw(buffer, pcmSamples, PLAY_SAMPLE_RATE, false);
    }

    // Non-blocking SD chunked playback (call each frame in SPEAKING)
    bool playChunk() {
        if (!_playingOpen) return false;
        if (_playPlayed >= _playTotalSamples) {
            _playFile.close();
            _playingOpen = false;
            Serial.printf("[AUDIO] playSD done: %zu/%zu samples\n", _playPlayed, _playTotalSamples);
            M5Cardputer.Speaker.stop();
            return false;
        }
        // Only feed next chunk when speaker buffer is free
        if (!M5Cardputer.Speaker.isPlaying()) {
            size_t want = (_playTotalSamples - _playPlayed > PLAY_CHUNK_SAMPLES)
                          ? PLAY_CHUNK_SAMPLES : _playTotalSamples - _playPlayed;
            size_t bytesRead = _playFile.read((uint8_t*)buffer, want * sizeof(int16_t));
            size_t got = bytesRead / sizeof(int16_t);
            if (got == 0) {
                _playFile.close();
                _playingOpen = false;
                M5Cardputer.Speaker.stop();
                return false;
            }
            M5Cardputer.Speaker.playRaw(buffer, got, PLAY_SAMPLE_RATE, false);
            _playPlayed += got;
        }
        return true;
    }

    // Start non-blocking SD playback (call once, then playChunk() each frame)
    void playSD(const char* path) {
        if (!sdAvailable) return;
        _playFile = SD.open(path, FILE_READ);
        if (!_playFile) {
            Serial.printf("[AUDIO] SD play: can't open %s\n", path);
            return;
        }
        size_t fileSize = _playFile.size();
        _playTotalSamples = fileSize / sizeof(int16_t);
        _playPlayed = 0;
        _playingOpen = true;
        Serial.printf("[AUDIO] playSD: %s %zu bytes (%.1fs @ %dHz)\n",
                      path, fileSize, (float)_playTotalSamples / PLAY_SAMPLE_RATE, PLAY_SAMPLE_RATE);
    }

    bool isPlaying() const {
        return M5Cardputer.Speaker.isPlaying();
    }

    void stopPlayback() {
        M5Cardputer.Speaker.stop();
    }

    // ── Status display (bottom of screen, 20-char limit) ────
    char statusText[24] = "";

    void showStatus(const char* text) {
        // Calculate how many chars fit on screen (6px per char at textSize 1)
        const int maxChars = DISPLAY_W / 6;  // 135/6 = 22
        int textLen = strlen(text);

        char newText[24];
        if (textLen > maxChars) {
            int keep = maxChars - 3;  // room for "..."
            strncpy(newText, text, keep);
            newText[keep] = '\0';
            strcat(newText, "...");
        } else {
            strncpy(newText, text, sizeof(newText) - 1);
            newText[sizeof(newText) - 1] = '\0';
        }

        // Skip if unchanged
        if (strcmp(statusText, newText) == 0) return;
        strcpy(statusText, newText);

        // Short beep on change
        M5Cardputer.Speaker.tone(880, 60);
    }

    // ── Get recording data for network upload ─────────────
    // For SD mode: read file into DRAM buffer.
    // Returns nullptr if too large (caller should stream from SD).
    int16_t* getRecordData(size_t& outBytes) {
        if (!useSD) {
            outBytes = recordedSamples * sizeof(int16_t);
            return buffer;
        }
        size_t fileBytes = recordedSamples * sizeof(int16_t);
        if (fileBytes <= DRAM_BYTES) {
            File f = SD.open(SD_RECORD_FILE, FILE_READ);
            if (!f) { outBytes = 0; return nullptr; }
            f.read((uint8_t*)buffer, fileBytes);
            f.close();
            outBytes = fileBytes;
            return buffer;
        }
        outBytes = fileBytes;
        return nullptr;
    }

    // ── Display ──────────────────────────────────────────
    int  cycleCount = 0;
    bool heartbeat  = false;
    unsigned long lastBeat = 0;
    char thinkLabelBuf[16];

    State lastDrawnState = State::AWAKE;  // start as non-SLEEP to force first draw

    void drawState(State state) {
        auto& dsp = M5Cardputer.Display;
        int w = dsp.width();
        int h = dsp.height();

        bool stateChanged = (state != lastDrawnState);
        lastDrawnState = state;

        // ── Static layout (only on state change) ─────────
        if (stateChanged) {
            dsp.fillScreen(TFT_BLACK);

            // Clear status text on transition to SLEEP
            if (state == State::SLEEP) {
                statusText[0] = '\0';
            }
        }

        // ── Dynamic indicators (dim when inactive, never disappear) ──
        {
            dsp.setTextSize(1);
            // WiFi: fixed x=2
            bool wifiOk = (WiFi.status() == WL_CONNECTED);
            dsp.fillRect(2, 2, 10, 8, TFT_BLACK);
            dsp.setTextColor(wifiOk ? 0x07E0 : 0x4208);  // green / dark grey
            dsp.drawString("W", 2, 2);
            // USB: fixed x=12
            bool usbOk = (network.transport == Transport::USB);
            dsp.fillRect(12, 2, 26, 8, TFT_BLACK);
            dsp.setTextColor(usbOk ? TFT_CYAN : 0x4208);  // cyan / dark grey
            dsp.drawString("USB", 12, 2);
            // SD: fixed x=40
            dsp.fillRect(40, 2, 18, 8, TFT_BLACK);
            dsp.setTextColor(sdAvailable ? 0x07E0 : 0x4208);  // green / dark grey
            dsp.drawString("SD", 40, 2);
        }

        // ── Dynamic (every frame) ────────────────────────
        // Heartbeat dot
        if (millis() - lastBeat > 500) {
            heartbeat = !heartbeat;
            lastBeat = millis();
        }
        dsp.fillRect(w - 8, 2, 6, 6, heartbeat ? TFT_GREEN : TFT_DARKGREEN);

        // State label (centered)
        const char* label = "";
        uint16_t labelColor = TFT_WHITE;
        switch (state) {
            case State::SLEEP:     label = "SLEEP";     labelColor = 0x7BEF; break;
            case State::RECORDING: label = "RECORDING"; labelColor = TFT_RED;   break;
            case State::PROCESSING: label = "THINKING";  labelColor = TFT_YELLOW; break;
            case State::SPEAKING:  label = "REPLYING";  labelColor = TFT_CYAN;  break;
            case State::ERROR_WAIT:label = "ERROR";     labelColor = TFT_RED;   break;
            default: break;
        }
        // Only redraw label if it changed (avoids flicker on PROCESSING spinner)
        static char lastLabel[16] = "";
        if (stateChanged || strcmp(lastLabel, label) != 0) {
            int labelW = strlen(label) * 18;
            dsp.fillRect((w - labelW)/2 - 4, h/2 - 16, labelW + 8, 24, TFT_BLACK);
            dsp.setTextSize(3);
            dsp.setTextColor(labelColor);
            dsp.drawString(label, (w - labelW) / 2, h / 2 - 14);
            strcpy(lastLabel, label);
        }

        // Status text (bottom row) — always draw if non-empty
        if (statusText[0] != '\0') {
            dsp.setTextSize(1);
            dsp.setTextColor(0x8410);  // grey
            int sw = strlen(statusText) * 6;
            dsp.fillRect(0, h - 12, w, 12, TFT_BLACK);
            dsp.drawString(statusText, (w - sw) / 2, h - 11);
        }

        // Tab + Fn hint on SLEEP — bottom left
        if (state == State::SLEEP) {
            dsp.fillRect(0, h - 12, w, 10, TFT_BLACK);
            dsp.setTextSize(1);
            dsp.setTextColor(0x4208);  // dark grey
            dsp.drawString("Tab:Setup  Fn:Chat", 2, h - 12);
        }
    }
};
