# ClawVoice

Voice-first AI assistant on M5Cardputer (ESP32-S3). Press Fn → speak → hear response.

Powered by [Hermes Agent](https://hermes-agent.nousresearch.com) for LLM reasoning and Volcengine (火山引擎) for speech recognition/synthesis.

## Architecture

```
 ┌─────────────────┐          ┌──────────────────────┐          ┌─────────────────┐
 │  Cardputer      │   WAV    │  Mac / Linux         │   HTTP   │  Hermes Agent   │
 │  (ESP32-S3)     │ ◄──────► │  proxy.py :8900      │ ◄──────► │  :8642 (api)    │
 │                 │   PCM    │                      │          │                 │
 │  Mic / Speaker  │          │  ASR → Chat → TTS    │  WebSocket         │  Volcengine      │
 │  Display / Keyb │          │                      │ ◄──────► │  ASR + TTS       │
 └─────────────────┘          └──────────────────────┘          └─────────────────┘
```

- **Cardputer** — Records audio, plays PCM, displays status. No heavy compute.
- **proxy.py** (Mac/Linux server) — Handles ASR (Volcengine WebSocket), LLM chat (Hermes SSE streaming), TTS (Volcengine HTTP).
- **Hermes Agent** — Local LLM agent with tool calling capability (auto-approved by proxy).
- **Volcengine** — Cloud speech recognition (大模型流式语音识别) and synthesis (豆包语音合成 2.0).

Two transport modes:
- **WiFi** — Cardputer talks directly to proxy over HTTP (requires 2.4GHz network).
- **USB Serial** — Cardputer ↔ usb_bridge.py ↔ proxy. Lower latency, no WiFi needed.

## Hardware Requirements

| Item | Details |
|------|---------|
| M5Stack Cardputer | ESP32-S3, recommended version with SD card slot (Cardputer ADV) |
| MicroSD card | For WiFi config persistence and extended recording (>4s) |
| Host computer | Mac or Linux on the same LAN (for proxy.py) |
| USB-C cable | For flashing firmware and USB transport mode |

## Quick Start

### Prerequisites

```bash
# 1. Python 3.9+ with venv
python3 --version

# 2. PlatformIO CLI (for ESP32 firmware)
# Install via Homebrew: brew install platformio
# Or: pip install platformio
pio --version

# 3. Hermes Agent (for LLM)
# Follow: https://hermes-agent.nousresearch.com/docs
# API server must be running on port 8642
hermes --version
```

### 1. Server Setup (Mac/Linux)

```bash
cd ~/Desktop/ClawVoice/server

# Install Python dependencies
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

# Set Volcengine credentials (required for speech)
export VOLC_APPID="your-app-id"
export VOLC_TOKEN="your-access-token"
export VOLC_SECRET="your-secret-key"

# Start proxy server
python proxy.py
# → Listening on http://0.0.0.0:8900
```

Test: `curl http://localhost:8900/health` → `{"status": "ok", "llm_backend": "hermes"}`

### 2. Cardputer Firmware

```bash
cd ~/Desktop/ClawVoice

# Build & flash (first build downloads ESP32 toolchain ~440MB)
pio run -t upload

# Monitor serial output
pio device monitor --baud 115200
```

**First build behind firewall?** Set proxy:
```bash
export HTTP_PROXY=http://127.0.0.1:7897 HTTPS_PROXY=http://127.0.0.1:7897
pio run -t upload
```

### 3. USB Bridge (Recommended)

USB bridge provides lower latency and doesn't require WiFi on the Cardputer:

```bash
cd ~/Desktop/ClawVoice

# Auto-detect Cardputer serial port and start bridge
python run_bridge.py
# → Found Cardputer at /dev/tty.usbmodemXXX
# → Bridge ready
```

### 4. One-Click Start

Or use `start.sh` to launch both proxy and bridge:

```bash
cd ~/Desktop/ClawVoice
./start.sh
```

## Usage

### Basic Operation

| Action | What happens |
|--------|-------------|
| **Hold Fn** | Start recording (speak into mic) |
| **Release Fn** | Send audio to server → ASR → LLM → TTS → playback |
| **Hold Fn during playback** | Interrupt / stop speaking |
| **Tab** (in idle) | Open WiFi settings page |

### State Display

| Screen | Meaning |
|--------|---------|
| `Ready` | Idle, waiting for input |
| `Recording...` | Hold Fn, speaking into mic |
| `Thinking...` | Waiting for LLM response |
| `Tool: xxx` | LLM is using a tool |
| `Generating...` | LLM generating text |
| `Speaking...` | Playing TTS audio |

### WiFi Setup (On-Device)

1. **Startup mode**: Hold Fn while powering on → enters WiFi setup
2. **Runtime mode**: Press Tab in idle state → enters WiFi setup

In setup page:
- **Fn+;** (semicolon) / **Fn+.** (period) — Navigate fields
- **Tab** — Next field
- **Enter** — Confirm / Save
- **Del** — Backspace

Config saved to SD card at `/wifi.cfg`:
```
MyWiFi
password123
255
```
(SSID, password, volume 0-255, one per line)

## Configuration

### Proxy Server (Environment Variables)

| Variable | Default | Description |
|----------|---------|-------------|
| `VOLC_APPID` | — | Volcengine App ID |
| `VOLC_TOKEN` | — | Volcengine Access Token |
| `VOLC_SECRET` | — | Volcengine Secret Key |
| `LLM_BACKEND` | `hermes` | LLM backend: `hermes` or `openclaw` |
| `HERMES_HOST` | `127.0.0.1` | Hermes API server host |
| `HERMES_PORT` | `8642` | Hermes API server port |
| `TTS_VOICE` | `zh_female_vv_uranus_bigtts` | TTS voice name |
| `TTS_RESOURCE_ID` | `seed-tts-2.0` | TTS resource ID |
| `ASR_RESOURCE_ID` | `volc.seedasr.sauc.duration` | ASR resource ID |
| `PROXY_HOST` | `0.0.0.0` | Proxy listen address |
| `PROXY_PORT` | `8900` | Proxy listen port |

### Cardputer (Compile-Time)

Default WiFi credentials are set in `platformio.ini` build flags. Override via environment:

```bash
export WIFI_SSID="MyWiFi" WIFI_PASS="mypassword"
pio run -t upload
```

Once the device boots, WiFi config is loaded from SD card (`/wifi.cfg`), which can be changed via the on-device setup page.

**Proxy address** defaults to `192.168.123.234:8900`. Change in `src/config.h` if your Mac has a different LAN IP:

```cpp
#define PROXY_HOST "192.168.x.x"
#define PROXY_PORT "8900"
```

## API Endpoints

### `POST /voice`
Speech-to-speech pipeline.
- **In**: WAV or raw PCM audio (16kHz 16-bit mono)
- **Out**: Streaming — STATUS lines followed by `RECV:<len>\n` + PCM audio (8kHz 16-bit mono)
- **Pipeline**: ASR → Hermes Chat → TTS

### `POST /text`
Text-to-speech.
- **In**: `{"text": "你好世界"}`
- **Out**: PCM audio (8kHz 16-bit mono)

### `GET /health`
```json
{"status": "ok", "llm_backend": "hermes", "hermes": "127.0.0.1:8642", ...}
```

## Project Structure

```
ClawVoice/
├── platformio.ini            # PIO build config
├── start.sh                  # One-click launcher (proxy + bridge)
├── run_bridge.py             # USB bridge wrapper
├── src/
│   ├── main.cpp              # State machine + setup
│   ├── config.h              # Constants, timing, pinouts
│   ├── audio.h               # Mic, speaker, waveform display
│   ├── network.h             # HTTP client (WiFi + USB transport)
│   └── settings.h            # SD config persistence + setup page UI
├── server/
│   ├── proxy.py              # ASR + LLM + TTS proxy server
│   ├── usb_bridge.py         # Serial ↔ HTTP bridge
│   ├── pre_upload.py         # Auto-kill usb_bridge before flash
│   ├── requirements.txt      # Python deps
│   └── test_*.py             # Quick smoke tests
├── test_burn/                # Minimal test firmware
└── test_arrows/              # Key mapping test
```

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| **"WIFI FAIL" on screen** | Wrong SSID/password, or 5GHz network | Check `/wifi.cfg` on SD card. 2.4GHz only. |
| **"No WiFi" / "Server error"** | proxy.py not running or wrong IP | `curl http://MAC_IP:8900/health` from Cardputer's network |
| **"USB error"** | usb_bridge.py not running or serial port conflict | Check `python run_bridge.py` output. Kill any process holding the serial port. |
| **"No speech detected"** | Mic too quiet or Volcengine auth failure | Check proxy.py logs. Verify VOLC_* env vars. |
| **ASR timeout / no response** | Volcengine network issue | Test: `curl http://localhost:8900/health` to verify ASR resource ID. |
| **"NO RAM!" at boot** | DRAM allocation failed | Reboot Cardputer. Ensure Serial buffer is not consuming too much RAM. |
| **Flash fails** | usb_bridge holding serial port | `pkill -f usb_bridge` before flashing. `pre_upload.py` does this automatically. |
| **Build fails (toolchain)** | First build needs ~440MB download | Ensure internet access. Set HTTP_PROXY if behind firewall. |
| **Tool calls hang forever** | Hermes requires approval for tools | **Fixed in `feat/auto-approve-tools`** — proxy auto-approves all tool calls. |
| **SD card not detected** | Wrong pins or no SD inserted | Only Cardputer ADV has SD slot. Check pins in `config.h`. |

## Hermes Tool Call Behavior

When Hermes decides to use a tool (e.g., web search, calendar), the API sends an `approval.request` event that normally requires manual confirmation. ClawVoice proxy auto-approves all tool calls with `choice: "always"` — the LLM can use any available tool without interruption. Tool progress is shown on the Cardputer screen (e.g., `Tool: web_search`).
