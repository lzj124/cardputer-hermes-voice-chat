# ClawVoice

Voice-first AI assistant on M5Cardputer (ESP32-S3). Press button → speak → hear response. The screen shows only a white waveform line.

## Architecture

```
Cardputer                    Mac Mini (proxy.py)              Cloud
┌──────────┐   WAV audio    ┌──────────────┐   WebSocket    ┌──────────────┐
│  Mic     │ ──────────────→│  /voice      │ ─────────────→ │ Volcengine   │
│  Speaker │ ←──────────────│  ASR → Chat  │                │ ASR (语音识别)│
│  Display │   PCM audio    │  → TTS       │ ←───────────── │ OpenClaw GW  │
│  Button  │                │  /text       │   HTTP/SSE     │ Volcengine   │
└──────────┘                └──────────────┘                │ TTS (语音合成)│
                                                            └──────────────┘
```

- **Cardputer**: Records audio, plays PCM, shows waveform, button input only
- **Mac Mini proxy**: Handles the heavy lifting — ASR (WebSocket binary), OpenClaw chat (SSE streaming), TTS (HTTP)
- **Cloud**: Volcengine ASR/TTS + OpenClaw gateway

## Quick Start

### 1. Mac Mini — Start proxy server

```bash
cd ~/Desktop/ClawVoice/server
pip install -r requirements.txt

# Set OpenClaw token (required for chat)
export OPENCLAW_TOKEN="your-openclaw-token"

# Start server
python proxy.py
# → Listening on http://0.0.0.0:8900
```

Test: `curl http://192.168.123.234:8900/health`

### 2. Cardputer — Build & flash

```bash
cd ~/Desktop/ClawVoice

# Set WiFi + proxy config
export WIFI_SSID="PDCN" WIFI_PASS="15011501" \
       PROXY_HOST="192.168.123.234" PROXY_PORT="8900" \
       OPENCLAW_TOKEN="your-openclaw-token"

# Build (first time needs proxy for toolchain download)
# export HTTP_PROXY=http://127.0.0.1:7897 HTTPS_PROXY=http://127.0.0.1:7897
~/Library/Python/3.9/bin/pio run -t upload
```

### 3. Use

1. Cardputer boots → shows breathing wave (sleep mode)
2. Press **Fn key** → beep → awake
3. **Hold Fn** → recording (waveform shows mic input)
4. **Release Fn** → sends audio to proxy → processing dots
5. Response plays through speaker → waveform animates
6. Returns to sleep

## State Machine

```
SLEEP ──[Fn press]──→ AWAKE ──[hold Fn]──→ RECORDING
  ↑                                              │
  │                          [release / timeout]  │
  │                                              ↓
  └── SPEAKING ←── PROCESSING ←──[send to proxy]─┘
        │
        [playback done / Fn press to interrupt]
```

## Configuration

### Cardputer (build flags in platformio.ini)

| Flag | Default | Description |
|------|---------|-------------|
| `WIFI_SSID` | PDCN | WiFi network |
| `WIFI_PASS` | — | WiFi password |
| `PROXY_HOST` | 192.168.123.234 | Mac Mini LAN IP |
| `PROXY_PORT` | 8900 | Proxy server port |
| `OPENCLAW_TOKEN` | — | OpenClaw gateway token |

### Proxy Server (env vars)

| Var | Default | Description |
|-----|---------|-------------|
| `VOLC_APPID` | 7443062928 | Volcengine App ID |
| `VOLC_TOKEN` | — | Volcengine Access Token |
| `VOLC_SECRET` | — | Volcengine Secret Key |
| `OPENCLAW_HOST` | 192.168.123.234 | OpenClaw gateway host |
| `OPENCLAW_PORT` | 18789 | OpenClaw gateway port |
| `OPENCLAW_TOKEN` | — | OpenClaw gateway token |
| `TTS_VOICE` | BV001_streaming | TTS voice type |
| `PROXY_PORT` | 8900 | Proxy listen port |

## API Endpoints (proxy.py)

### `POST /voice`
- **In**: Raw PCM audio (16kHz 16-bit mono)
- **Out**: Raw PCM audio (8kHz 16-bit mono)
- **Pipeline**: ASR → OpenClaw Chat → TTS

### `POST /text`
- **In**: `{"text": "..."}`
- **Out**: Raw PCM audio (8kHz 16-bit mono)
- **Pipeline**: OpenClaw Chat → TTS

### `GET /health`
- Returns JSON with server status and config

## File Structure

```
ClawVoice/
├── platformio.ini          # PIO build config
├── src/
│   ├── main.cpp            # State machine + Arduino setup/loop
│   ├── config.h            # Constants, WiFi, timing
│   ├── audio.h             # Mic recording, speaker playback, waveform display
│   └── network.h           # HTTP client (POST /voice, POST /text)
├── server/
│   ├── proxy.py            # Mac Mini proxy: ASR + TTS + OpenClaw
│   └── requirements.txt    # Python deps (flask, requests, websocket-client)
└── README.md
```

## Troubleshooting

- **"WIFI FAIL"**: Check SSID/password, ensure 2.4GHz network
- **No response**: Verify proxy.py is running and `PROXY_HOST` is correct
- **ASR empty**: Speak louder/closer, check Volcengine credentials
- **Build fails**: First build needs internet (toolchain ~440MB). Set `HTTP_PROXY` if behind firewall
