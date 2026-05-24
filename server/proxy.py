#!/usr/bin/env python3
"""
ClawVoice Proxy Server — runs on Mac Mini
Bridges Cardputer ↔ Volcengine ASR/TTS + OpenClaw Gateway

Endpoints:
  POST /voice  — WAV/PCM audio → PCM audio  (ASR → Chat → TTS pipeline)
  POST /text   — JSON {"text":"..."} → PCM audio  (Chat → TTS pipeline)

Usage:
  pip install -r requirements.txt
  python proxy.py
"""

import os, json, uuid, struct, base64, logging, argparse, io, wave
from flask import Flask, request, jsonify, Response
import requests
import websocket

# ── Configuration ──────────────────────────────────────────────
VOLCENGINE_APP_ID       = os.getenv("VOLC_APPID", "7443062928")
VOLCENGINE_ACCESS_TOKEN = os.getenv("VOLC_TOKEN", "KSuWo2PcxSGZEPk9ODiGk3usV2XAQv3-")
VOLCENGINE_SECRET_KEY   = os.getenv("VOLC_SECRET", "Ut_DV6muixCck9edHsL6fxYTA4nUUAvn")

# ── LLM Backend ──────────────────────────────────────────────
LLM_BACKEND = os.getenv("LLM_BACKEND", "hermes")  # "hermes" or "openclaw"

OPENCLAW_HOST  = os.getenv("OPENCLAW_HOST", "192.168.123.234")
OPENCLAW_PORT  = os.getenv("OPENCLAW_PORT", "18789")
OPENCLAW_TOKEN = os.getenv("OPENCLAW_TOKEN", "clawvoice2026")

# TTS settings — 豆包语音合成 2.0 (V3 HTTP Chunked)
TTS_URL         = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
TTS_RESOURCE_ID = os.getenv("TTS_RESOURCE_ID", "seed-tts-2.0")
TTS_VOICE       = os.getenv("TTS_VOICE", "zh_female_vv_uranus_bigtts")  # vivi 2.0

# ASR settings — 大模型流式语音识别 2.0 (V3 WebSocket)
ASR_URL        = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream"
ASR_RESOURCE_ID = os.getenv("ASR_RESOURCE_ID", "volc.seedasr.sauc.duration")

# Output audio format for Cardputer
OUTPUT_SAMPLE_RATE = 8000  # ESP32 speaker plays 8kHz PCM
OUTPUT_FORMAT      = "pcm"

HOST = os.getenv("PROXY_HOST", "0.0.0.0")
PORT = int(os.getenv("PROXY_PORT", "8900"))

app = Flask(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("clawvoice")


# ── Volcengine ASR (V3 WebSocket binary protocol) ─────────────

def _asr_build_header(msg_type, flags, serialization=0, compression=0):
    """Build 4-byte binary header for WebSocket binary protocol."""
    return bytes([
        0x11,                          # protocol_version=1, header_size=1(4B)
        (msg_type << 4) | flags,
        (serialization << 4) | compression,
        0x00,                          # reserved
    ])

def _asr_parse_response(msg):
    """Parse V3 server response: header(4B) + sequence(4B) + payload_size(4B) + payload."""
    if not isinstance(msg, bytes) or len(msg) < 12:
        return None, None, None
    msg_type = (msg[1] >> 4) & 0x0F
    flags = msg[1] & 0x0F
    seq = struct.unpack(">I", msg[4:8])[0]
    psz = struct.unpack(">I", msg[8:12])[0]
    payload = msg[12:12 + psz] if psz <= len(msg) - 12 else msg[12:]
    return msg_type, flags, payload

def asr_recognize(audio_bytes):
    """
    Send audio (WAV or raw PCM) to Volcengine ASR V3, return recognized text.
    Uses bigmodel_nostream (流式输入模式) — sends all audio, waits for result.
    """
    # Convert to WAV if raw PCM
    if audio_bytes[:4] == b'RIFF':
        wav_data = audio_bytes
    else:
        # Wrap raw PCM into WAV
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(audio_bytes)
        wav_data = buf.getvalue()

    result_text = ""

    try:
        ws = websocket.WebSocket()
        ws.settimeout(15)
        ws.connect(
            ASR_URL,
            header={
                "X-Api-App-Key": VOLCENGINE_APP_ID,
                "X-Api-Access-Key": VOLCENGINE_ACCESS_TOKEN,
                "X-Api-Resource-Id": ASR_RESOURCE_ID,
                "X-Api-Request-Id": str(uuid.uuid4()),
                "X-Api-Sequence": "-1",
            },
        )
        log.info(f"ASR V3 connected, sending {len(wav_data)} bytes WAV")

        # Full client request (JSON config)
        config = json.dumps({
            "user": {"uid": "clawvoice"},
            "audio": {
                "format": "wav",
                "rate": 16000,
                "bits": 16,
                "channel": 1,
                "language": "zh-CN",
            },
            "request": {
                "model_name": "bigmodel",
            },
        })
        config_bytes = config.encode("utf-8")
        frame = _asr_build_header(0x01, 0x00, serialization=1) + \
                struct.pack(">I", len(config_bytes)) + config_bytes
        ws.send(frame, opcode=websocket.ABNF.OPCODE_BINARY)

        # Audio data as last frame (flags=0x02 = negative/last packet)
        frame = _asr_build_header(0x02, 0x02) + \
                struct.pack(">I", len(wav_data)) + wav_data
        ws.send(frame, opcode=websocket.ABNF.OPCODE_BINARY)

        # Read responses
        while True:
            try:
                msg = ws.recv()
            except websocket.WebSocketTimeoutException:
                log.warning("ASR response timeout")
                break

            msg_type, flags, payload = _asr_parse_response(msg)
            if msg_type is None:
                continue

            if msg_type == 0x09:  # full server response
                try:
                    resp = json.loads(payload.decode("utf-8"))
                except:
                    continue

                text = resp.get("result", {}).get("text", "")
                if text:
                    result_text = text

                # flags=0x03 means last response
                if flags == 0x03:
                    break

            elif msg_type == 0x0F:  # error
                try:
                    err = json.loads(payload.decode("utf-8"))
                    log.error(f"ASR V3 error: {err}")
                except:
                    log.error(f"ASR V3 error (raw): {payload[:200]}")
                break

        ws.close()

    except Exception as e:
        log.error(f"ASR exception: {e}")
        return ""

    log.info(f"ASR result: '{result_text}'")
    return result_text.strip()


# ── Volcengine TTS 2.0 (V3 HTTP Chunked) ───────────────────────

def tts_synthesize(text):
    """
    Call 豆包语音合成 2.0 V3 HTTP Chunked API.
    Returns PCM audio bytes at OUTPUT_SAMPLE_RATE, or None on failure.
    """
    headers = {
        "Content-Type": "application/json",
        "X-Api-App-Key": VOLCENGINE_APP_ID,
        "X-Api-Access-Key": VOLCENGINE_ACCESS_TOKEN,
        "X-Api-Resource-Id": TTS_RESOURCE_ID,
        "X-Api-Request-Id": str(uuid.uuid4()),
    }

    payload = {
        "user": {"uid": "clawvoice"},
        "namespace": "BidirectionalTTS",
        "req_params": {
            "text": text,
            "speaker": TTS_VOICE,
            "audio_params": {
                "format": OUTPUT_FORMAT,
                "sample_rate": OUTPUT_SAMPLE_RATE,
            },
        },
    }

    try:
        resp = requests.post(TTS_URL, json=payload, headers=headers, timeout=30, stream=True)
        if resp.status_code != 200:
            log.error(f"TTS HTTP {resp.status_code}: {resp.text[:300]}")
            return None

        # V3 returns chunked JSON, each chunk has a "data" field with base64 audio
        pcm_chunks = bytearray()

        for line in resp.iter_lines():
            if not line:
                continue
            line = line.decode("utf-8", errors="ignore")
            if not line.startswith("{"):
                continue
            try:
                chunk = json.loads(line)
                code = chunk.get("code", -1)
                if code not in (0, 20000000):
                    log.error(f"TTS chunk error {code}: {chunk.get('message', '')}")
                    continue

                data = chunk.get("data", "")
                if data and isinstance(data, str):
                    pcm_chunks.extend(base64.b64decode(data))

                # Check for session finish events
                event = chunk.get("event", 0)
                if event in (152, 153):  # SessionFinish or SessionFailed
                    break

            except json.JSONDecodeError:
                pass

        if not pcm_chunks:
            log.error("TTS: no audio data received")
            return None

        log.info(f"TTS synthesized {len(pcm_chunks)} bytes PCM ({len(pcm_chunks)/2/OUTPUT_SAMPLE_RATE:.1f}s)")
        return bytes(pcm_chunks)

    except Exception as e:
        log.error(f"TTS exception: {e}")
        return None


# ── Hermes Gateway (API Server) ───────────────────────────────

HERMES_HOST  = os.getenv("HERMES_HOST", "127.0.0.1")
HERMES_PORT  = os.getenv("HERMES_PORT", "8642")
HERMES_TOKEN = os.getenv("HERMES_TOKEN", "")  # optional auth

SYSTEM_PROMPT = (
    "你是一个语音助手，通过 ClawVoice 与用户对话。"
    "用户通过语音听到你的回答，尽量简洁。"
    "用纯文本回复，不要使用 emoji 或 markdown。"
)


def hermes_chat(text):
    """Send text to Hermes /v1/runs, return response text (no tool progress)."""
    if not text.strip():
        return ""

    url = f"http://{HERMES_HOST}:{HERMES_PORT}/v1/runs"
    headers = {"Content-Type": "application/json"}
    if HERMES_TOKEN:
        headers["Authorization"] = f"Bearer {HERMES_TOKEN}"

    payload = {
        "input": text,
        "instructions": SYSTEM_PROMPT,
    }

    try:
        resp = requests.post(url, json=payload, headers=headers, timeout=10)
        if resp.status_code not in (200, 202):
            log.error(f"Hermes POST /v1/runs HTTP {resp.status_code}: {resp.text[:200]}")
            return ""

        data = resp.json()
        run_id = data.get("run_id") or data.get("id", "")
        if not run_id:
            log.error(f"Hermes: no run_id in response: {data}")
            return ""

        # Stream events via SSE
        events_url = f"http://{HERMES_HOST}:{HERMES_PORT}/v1/runs/{run_id}/events"
        ev_resp = requests.get(events_url, headers=headers, timeout=120, stream=True)
        if ev_resp.status_code != 200:
            log.error(f"Hermes GET events HTTP {ev_resp.status_code}")
            return ""

        full_text = ""
        for line in ev_resp.iter_lines():
            if not line:
                continue
            line = line.decode("utf-8", errors="ignore")
            if not line.startswith("data: "):
                continue
            payload_str = line[6:]
            try:
                ev = json.loads(payload_str)
            except json.JSONDecodeError:
                continue

            event_type = ev.get("event", "")
            if event_type == "message.delta":
                delta = ev.get("delta", "")
                if delta:
                    full_text += delta
            elif event_type == "run.completed":
                # Final output may be in "output" field
                output = ev.get("output", "")
                if output and not full_text:
                    full_text = output
                break
            elif event_type == "run.failed":
                log.error(f"Hermes run failed: {ev.get('error', 'unknown')}")
                break

        result = full_text.strip()[:300]
        log.info(f"Hermes response ({len(result)} chars): {result[:80]}...")
        return result

    except Exception as e:
        log.error(f"Hermes exception: {e}")
        return ""


def hermes_chat_stream(text, status_callback=None):
    """
    Send text to Hermes /v1/runs, yield status events during processing.
    Returns (response_text, tool_events) when done.

    Yields dicts:
      {"type": "status", "text": "..."}  — show on ESP32 screen
      {"type": "tool", "name": "...", "state": "started|completed"}
    """
    if not text.strip():
        return ""

    url = f"http://{HERMES_HOST}:{HERMES_PORT}/v1/runs"
    headers = {"Content-Type": "application/json"}
    if HERMES_TOKEN:
        headers["Authorization"] = f"Bearer {HERMES_TOKEN}"

    payload = {
        "input": text,
        "instructions": SYSTEM_PROMPT,
    }

    try:
        resp = requests.post(url, json=payload, headers=headers, timeout=10)
        if resp.status_code not in (200, 202):
            log.error(f"Hermes POST /v1/runs HTTP {resp.status_code}: {resp.text[:200]}")
            return ""

        data = resp.json()
        run_id = data.get("run_id") or data.get("id", "")
        if not run_id:
            log.error(f"Hermes: no run_id in response: {data}")
            return ""

        events_url = f"http://{HERMES_HOST}:{HERMES_PORT}/v1/runs/{run_id}/events"
        ev_resp = requests.get(events_url, headers=headers, timeout=120, stream=True)
        if ev_resp.status_code != 200:
            log.error(f"Hermes GET events HTTP {ev_resp.status_code}")
            return ""

        full_text = ""
        for line in ev_resp.iter_lines():
            if not line:
                continue
            line = line.decode("utf-8", errors="ignore")
            if not line.startswith("data: "):
                continue
            payload_str = line[6:]
            try:
                ev = json.loads(payload_str)
            except json.JSONDecodeError:
                continue

            event_type = ev.get("event", "")

            if event_type == "tool.started":
                tool_name = ev.get("tool", "unknown")
                preview = ev.get("preview", "")
                label = preview if preview else tool_name
                # Truncate for ESP32 display (20 chars max)
                if len(label) > 20:
                    label = label[:18] + ".."
                yield {"type": "status", "text": f"Tool: {label}"}

            elif event_type == "tool.completed":
                tool_name = ev.get("tool", "")
                yield {"type": "status", "text": f"Done: {tool_name}"}

            elif event_type == "reasoning.available":
                thinking = ev.get("text", "")
                if thinking:
                    short = thinking[:18] + (".." if len(thinking) > 18 else "")
                    yield {"type": "status", "text": short}

            elif event_type == "message.delta":
                delta = ev.get("delta", "")
                if delta:
                    if not full_text:
                        yield {"type": "status", "text": "Generating..."}
                    full_text += delta

            elif event_type == "run.completed":
                output = ev.get("output", "")
                if output and not full_text:
                    full_text = output
                break

            elif event_type == "run.failed":
                log.error(f"Hermes run failed: {ev.get('error', 'unknown')}")
                break

        result = full_text.strip()[:300]
        log.info(f"Hermes response ({len(result)} chars): {result[:80]}...")
        return result

    except Exception as e:
        log.error(f"Hermes exception: {e}")
        return ""


# ── OpenClaw Gateway ──────────────────────────────────────────

def openclaw_chat(text):
    """Send text to OpenClaw gateway, return response text."""
    if not text.strip():
        return ""

    url = f"http://{OPENCLAW_HOST}:{OPENCLAW_PORT}/v1/chat/completions"
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_TOKEN}",
    }

    payload = {
        "model": "openclaw",
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "max_tokens": 300,
    }

    try:
        resp = requests.post(url, json=payload, headers=headers, timeout=120)
        if resp.status_code != 200:
            log.error(f"OpenClaw HTTP {resp.status_code}: {resp.text[:200]}")
            return ""
        data = resp.json()
        content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
        result = content.strip()[:300]
        log.info(f"OpenClaw response ({len(result)} chars): {result[:80]}...")
        return result
    except Exception as e:
        log.error(f"OpenClaw exception: {e}")
        return ""


# ── Pipeline ───────────────────────────────────────────────────

def pipeline_voice_stream(audio_bytes):
    """Streaming voice pipeline: yields STATUS lines during Chat, then RECV header + PCM."""
    # Step 1: ASR
    yield "STATUS:Listening...\n"
    text = asr_recognize(audio_bytes)
    if not text:
        yield "STATUS:No speech detected\n"
        yield "RECV:0\n"
        return

    preview = text[:18] + (".." if len(text) > 18 else "")
    yield f"STATUS:Heard: {preview}\n"

    # Step 2: Chat via configured LLM backend
    yield "STATUS:Thinking...\n"

    if LLM_BACKEND == "openclaw":
        response = openclaw_chat(text)
    else:
        response = ""
        gen = hermes_chat_stream(text)
        try:
            while True:
                event = next(gen)
                if isinstance(event, dict) and event.get("type") == "status":
                    yield f"STATUS:{event['text']}\n"
        except StopIteration as e:
            response = e.value or ""

    if not response:
        response = "抱歉，我没听懂。"

    # Step 3: TTS
    yield "STATUS:Speaking...\n"
    pcm = tts_synthesize(response)
    if pcm:
        yield f"RECV:{len(pcm)}\n"
        yield pcm
    else:
        yield "RECV:0\n"


def pipeline_text(text):
    """Text pipeline: text → Chat → TTS → PCM"""
    if not text.strip():
        return None
    if LLM_BACKEND == "openclaw":
        response = openclaw_chat(text)
    else:
        response = hermes_chat(text)
    if not response:
        response = "抱歉，处理失败了。"
    return tts_synthesize(response)


# ── HTTP Endpoints ─────────────────────────────────────────────

@app.route("/voice", methods=["POST"])
def handle_voice():
    audio_data = request.get_data()
    if not audio_data or len(audio_data) < 10:
        return jsonify({"error": "audio too short"}), 400

    log.info(f"/voice received {len(audio_data)} bytes")

    def generate():
        for chunk in pipeline_voice_stream(audio_data):
            if isinstance(chunk, str):
                yield chunk.encode("utf-8")
            else:
                yield chunk

    return Response(generate(), mimetype="application/octet-stream")


@app.route("/text", methods=["POST"])
def handle_text():
    data = request.get_json(force=True)
    text = data.get("text", "").strip()
    if not text:
        return jsonify({"error": "empty text"}), 400

    log.info(f"/text received: {text[:80]}")

    pcm = pipeline_text(text)

    if pcm:
        return Response(pcm, mimetype="audio/pcm")
    else:
        return Response(b"", mimetype="audio/pcm")


@app.route("/health", methods=["GET"])
def health():
    return jsonify({
        "status": "ok",
        "llm_backend": LLM_BACKEND,
        "volcengine_appid": VOLCENGINE_APP_ID,
        "hermes": f"{HERMES_HOST}:{HERMES_PORT}",
        "tts_voice": TTS_VOICE,
        "tts_resource_id": TTS_RESOURCE_ID,
        "asr_resource_id": ASR_RESOURCE_ID,
    })


# ── Main ───────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ClawVoice Proxy Server")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    log.info(f"Starting ClawVoice proxy on {args.host}:{args.port}")
    log.info(f"  Volcengine APP ID: {VOLCENGINE_APP_ID}")
    log.info(f"  ASR: 大模型流式语音识别 V3, resource={ASR_RESOURCE_ID}")
    log.info(f"  TTS: 豆包语音合成 2.0, voice={TTS_VOICE}, resource={TTS_RESOURCE_ID}")
    log.info(f"  LLM: Hermes {HERMES_HOST}:{HERMES_PORT} (tool progress enabled)")
    if LLM_BACKEND == "openclaw":
        log.warning("  ⚠ LLM_BACKEND=openclaw (legacy, no tool progress)")

    app.run(host=args.host, port=args.port, debug=args.debug, use_reloader=False, threaded=True)
