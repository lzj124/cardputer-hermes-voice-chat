#!/usr/bin/env python3
"""
ClawVoice — ASR & TTS standalone test
Tests Volcengine ASR (WebSocket) and TTS 2.0 (V3 HTTP Chunked) independently.
"""

import json, uuid, struct, base64, sys, os, wave, io

# Add parent for imports
VOLC_APPID = "7443062928"
VOLC_TOKEN = "VOLC_TOKEN_REDACTED"
VOLC_SECRET = "VOLC_SECRET_REDACTED"

# ── Test TTS 2.0 ─────────────────────────────────────────────

def test_tts(text="你好，我是ClawVoice语音助手，很高兴认识你。"):
    """Test 豆包语音合成 2.0 V3 HTTP Chunked"""
    import requests

    print(f"\n{'='*50}")
    print(f"[TTS TEST] Text: {text}")
    print(f"{'='*50}")

    url = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
    headers = {
        "Content-Type": "application/json",
        "X-Api-Key": VOLC_TOKEN,
        "X-Api-Resource-Id": "seed-tts-2.0",
        "X-Api-Request-Id": str(uuid.uuid4()),
    }
    payload = {
        "user": {"uid": "clawvoice-test"},
        "namespace": "BidirectionalTTS",
        "req_params": {
            "text": text,
            "speaker": "zh_female_vv_uranus_bigtts",
            "audio_params": {
                "format": "pcm",
                "sample_rate": 8000,
            },
        },
    }

    print(f"  URL: {url}")
    print(f"  Headers: X-Api-Key=...{VOLC_TOKEN[-8:]}, Resource-Id=seed-tts-2.0")
    print(f"  Voice: zh_female_vv_uranus_bigtts (vivi 2.0)")
    print(f"  Sending request...")

    try:
        resp = requests.post(url, json=payload, headers=headers, timeout=30, stream=True)
        print(f"  HTTP Status: {resp.status_code}")
        print(f"  Response Headers: {dict(resp.headers)}")

        if resp.status_code != 200:
            print(f"  ERROR: {resp.text[:500]}")
            return False

        pcm_chunks = bytearray()
        chunk_count = 0

        for line in resp.iter_lines():
            if not line:
                continue
            line_str = line.decode("utf-8", errors="ignore")
            if not line_str.strip().startswith("{"):
                print(f"  [non-JSON line] {line_str[:100]}")
                continue

            try:
                chunk = json.loads(line_str)
                chunk_count += 1
                code = chunk.get("code", -1)
                event = chunk.get("event", 0)
                message = chunk.get("message", "")
                data = chunk.get("data", "")

                if chunk_count <= 5 or code not in (0, 20000000):
                    print(f"  Chunk #{chunk_count}: code={code}, event={event}, "
                          f"msg={message[:60]}, data_len={len(data) if data else 0}")

                if code not in (0, 20000000):
                    print(f"  ERROR chunk: {json.dumps(chunk, ensure_ascii=False)[:300]}")
                    continue

                if data and isinstance(data, str):
                    decoded = base64.b64decode(data)
                    pcm_chunks.extend(decoded)

                # Session events: 152=finish, 153=failed
                if event in (152, 153):
                    print(f"  Session event: {event} ({'finish' if event==152 else 'failed'})")
                    break

            except json.JSONDecodeError as e:
                print(f"  [JSON parse error] {e}: {line_str[:100]}")

        duration = len(pcm_chunks) / 2 / 8000
        print(f"\n  Total chunks: {chunk_count}")
        print(f"  Total PCM bytes: {len(pcm_chunks)}")
        print(f"  Audio duration: {duration:.2f}s")

        if pcm_chunks:
            # Save as WAV for playback
            out_path = os.path.expanduser("~/Desktop/ClawVoice/test_tts.wav")
            with wave.open(out_path, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(8000)
                wf.writeframes(bytes(pcm_chunks))
            print(f"  Saved WAV: {out_path}")
            print(f"  ✅ TTS SUCCESS")
            return True
        else:
            print(f"  ❌ TTS FAILED: no audio data")
            return False

    except Exception as e:
        print(f"  ❌ TTS EXCEPTION: {e}")
        import traceback
        traceback.print_exc()
        return False


# ── Test ASR ──────────────────────────────────────────────────

def test_asr():
    """Test Volcengine ASR with a synthetic test WAV or a provided file"""
    import websocket

    print(f"\n{'='*50}")
    print(f"[ASR TEST]")
    print(f"{'='*50}")

    # Check if user provided a WAV file
    wav_path = None
    if len(sys.argv) > 1:
        wav_path = sys.argv[1]
    
    if wav_path and os.path.exists(wav_path):
        print(f"  Using WAV file: {wav_path}")
        with open(wav_path, "rb") as f:
            wav_data = f.read()
        if wav_data[:4] == b'RIFF':
            pcm_data = wav_data[44:]
            print(f"  WAV file: {len(wav_data)} bytes, PCM: {len(pcm_data)} bytes")
        else:
            pcm_data = wav_data
    else:
        # Generate 1s of silence (16kHz, 16-bit, mono) for a basic connection test
        print(f"  No WAV file provided, using 1s silence for connection test")
        print(f"  Usage: python test_asr_tts.py <audio.wav>")
        pcm_data = b'\x00\x00' * 16000  # 1s silence

    url = "wss://openspeech.bytedance.com/api/v2/asr"
    
    def build_header(msg_type, flags, serialization=0, compression=0):
        return bytes([
            0x11,
            (msg_type << 4) | flags,
            (serialization << 4) | compression,
            0x00,
        ])

    try:
        ws = websocket.WebSocket()
        ws.settimeout(15)

        print(f"  Connecting to {url}...")
        ws.connect(url, header={"Authorization": f"Bearer;{VOLC_TOKEN}"})
        print(f"  Connected!")

        # Send config
        config = json.dumps({
            "app": {
                "appid": VOLC_APPID,
                "token": VOLC_TOKEN,
                "cluster": "volcengine_streaming_common",
            },
            "user": {"uid": "clawvoice-test"},
            "audio": {
                "format": "raw",
                "rate": 16000,
                "bits": 16,
                "channel": 1,
                "language": "zh-CN",
            },
            "request": {
                "reqid": str(uuid.uuid4()),
                "workflow": "audio_in,resample,partition,vad,fe,decode",
                "sequence": -1,
                "nbest": 1,
                "show_utterances": True,
            },
        })
        config_bytes = config.encode("utf-8")
        frame = build_header(0x01, 0x00, serialization=1) + \
                struct.pack(">I", len(config_bytes)) + config_bytes
        print(f"  Sending config ({len(config_bytes)} bytes)...")
        ws.send(frame, opcode=websocket.ABNF.OPCODE_BINARY)

        # Send audio
        frame = build_header(0x02, 0x02) + \
                struct.pack(">I", len(pcm_data)) + pcm_data
        print(f"  Sending audio ({len(pcm_data)} bytes)...")
        ws.send(frame, opcode=websocket.ABNF.OPCODE_BINARY)

        # Read responses
        print(f"  Waiting for response...")
        result_text = ""
        msg_count = 0
        while True:
            try:
                msg = ws.recv()
            except websocket.WebSocketTimeoutException:
                print(f"  Timeout waiting for response")
                break

            msg_count += 1
            if not isinstance(msg, bytes) or len(msg) < 8:
                print(f"  Msg #{msg_count}: non-binary or too short")
                continue

            payload_size = struct.unpack(">I", msg[4:8])[0]
            payload = msg[8:8 + payload_size]
            msg_type = (msg[1] >> 4) & 0x0F

            if msg_type == 0x09:
                resp = json.loads(payload.decode("utf-8"))
                code = resp.get("code", -1)
                msg_text = resp.get("message", "")
                print(f"  Msg #{msg_count}: type=full_response, code={code}, msg={msg_text[:80]}")
                
                if code == 0 and "result" in resp:
                    for r in resp["result"]:
                        text = r.get("text", "")
                        if text:
                            result_text = text
                            print(f"  Recognized: {text}")
                
                # Check if final
                payload_msg = resp.get("payload_msg", resp)
                if payload_msg.get("is_final", False):
                    print(f"  Final result received")
                    break
                elif code != 0:
                    print(f"  Error response: {json.dumps(resp, ensure_ascii=False)[:300]}")
                    break

            elif msg_type == 0x0F:
                resp = json.loads(payload.decode("utf-8"))
                print(f"  Msg #{msg_count}: type=error, {json.dumps(resp, ensure_ascii=False)[:300]}")
                break
            else:
                print(f"  Msg #{msg_count}: type={msg_type:#x}, size={payload_size}")

        ws.close()

        if result_text:
            print(f"\n  ✅ ASR SUCCESS: '{result_text}'")
            return True
        elif msg_count > 0:
            print(f"\n  ⚠️ ASR connected OK but no text (silence input?)")
            return True  # connection works
        else:
            print(f"\n  ❌ ASR FAILED")
            return False

    except Exception as e:
        print(f"  ❌ ASR EXCEPTION: {e}")
        import traceback
        traceback.print_exc()
        return False


# ── Main ──────────────────────────────────────────────────────

if __name__ == "__main__":
    print("ClawVoice — ASR & TTS Test")
    print(f"Volcengine APP ID: {VOLC_APPID}")
    
    # Test TTS first (doesn't need input file)
    tts_ok = test_tts()
    
    # Test ASR
    asr_ok = test_asr()
    
    print(f"\n{'='*50}")
    print(f"Results: TTS={'✅' if tts_ok else '❌'}  ASR={'✅' if asr_ok else '❌'}")
    print(f"{'='*50}")
