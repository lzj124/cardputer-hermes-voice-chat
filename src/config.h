#pragma once
// ClawVoice — Compile-time & runtime configuration

// ── Transport ────────────────────────────────────────────────
enum class Transport { WIFI, USB };

// ── WiFi ────────────────────────────────────────────────────
#ifndef WIFI_SSID
#define WIFI_SSID "WIFI_SSID_REDACTED"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "WIFI_PASS_REDACTED"
#endif

// ── Proxy Server (Mac Mini) ─────────────────────────────────
#ifndef PROXY_HOST
#define PROXY_HOST "192.168.123.234"
#endif
#ifndef PROXY_PORT
#define PROXY_PORT "8900"
#endif
#ifndef PROXY_PORT_NUM
#define PROXY_PORT_NUM 8900
#endif

// ── Audio ───────────────────────────────────────────────────
// Cardputer mic records at 16kHz, 16-bit mono
// Proxy server resamples TTS output to 8kHz for playback
static constexpr int   MIC_SAMPLE_RATE   = 16000;
static constexpr int   MIC_BITS          = 16;
static constexpr int   MIC_CHANNEL       = 1;
static constexpr float MAX_RECORD_SEC_SD = 30.0f;   // max recording with SD card
static constexpr float MAX_RECORD_SEC_RAM = 4.0f;   // max recording without SD (DRAM limit)
static constexpr int   PLAY_SAMPLE_RATE  = 8000;    // speaker playback rate

// ── SD Card ─────────────────────────────────────────────────
// Cardputer ADV SD SPI pins (separate from display SPI!)
static constexpr int SD_SPI_SCK  = 40;
static constexpr int SD_SPI_MISO = 39;
static constexpr int SD_SPI_MOSI = 14;
static constexpr int SD_SPI_CS   = 12;
static constexpr const char* SD_RECORD_FILE = "/rec.pcm";
static constexpr const char* SD_TTS_FILE    = "/tts.pcm";

// ── State Machine ───────────────────────────────────────────
enum class State {
    SLEEP,       // Idle, screen shows status
    AWAKE,       // Just woke up (key press), ready to record
    RECORDING,   // Holding key, recording audio
    PROCESSING,  // Sent to proxy, waiting for response
    SPEAKING,    // Playing TTS audio back
    ERROR_WAIT,  // Error occurred, brief pause before returning to SLEEP
};

// ── Display ─────────────────────────────────────────────────
static constexpr int   DISPLAY_W       = 135;
static constexpr int   DISPLAY_H      = 240;
static constexpr int   WAVE_Y_CENTER  = 120;
static constexpr int   WAVE_AMP_MAX   = 50;

// ── Timing (ms) ─────────────────────────────────────────────
static constexpr unsigned long HTTP_TIMEOUT      = 120000; // 120s — wait for OpenClaw
static constexpr unsigned long USB_ALIVE_TIMEOUT  = 5000;   // 5s — USB heartbeat timeout for isUsbAlive()
static constexpr unsigned long ERROR_DISPLAY_MS  = 2000;   // show error 2s
static constexpr unsigned long SLEEP_AFTER_MS    = 60000;  // go to sleep after 60s idle
