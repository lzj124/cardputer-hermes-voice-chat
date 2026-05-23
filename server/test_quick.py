#!/usr/bin/env python3
"""Quick inline test: ASR + TTS via proxy.py functions"""

import signal, sys, os, wave, struct, json, io
sys.path.insert(0, os.path.dirname(__file__))

def handler(s,f): print('\nTIMEOUT', flush=True); sys.exit(2)
signal.signal(signal.SIGALRM, handler)
signal.alarm(30)

# ── Test ASR ──────────────────────────────────────────────
print("=" * 50, flush=True)
print("[1/2] Testing ASR V3 (silence → should return empty text)", flush=True)
print("=" * 50, flush=True)

from proxy import asr_recognize

# Generate 1s silence WAV
buf = io.BytesIO()
with wave.open(buf, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(b'\x00\x00' * 16000)
silence_wav = buf.getvalue()

text = asr_recognize(silence_wav)
if text:
    print(f"  ⚠️ Got unexpected text: '{text}'", flush=True)
else:
    print(f"  ✅ ASR connection OK (empty result = expected for silence)", flush=True)


# ── Test TTS ──────────────────────────────────────────────
print(f"\n{'=' * 50}", flush=True)
print("[2/2] Testing TTS 2.0 V3 (text → PCM audio)", flush=True)
print("=" * 50, flush=True)

from proxy import tts_synthesize

pcm = tts_synthesize("你好，我是小C，很高兴认识你。")
if pcm and len(pcm) > 100:
    duration = len(pcm) / 2 / 8000
    out_path = os.path.expanduser("~/Desktop/ClawVoice/test_tts_v2.wav")
    with wave.open(out_path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(pcm)
    print(f"  ✅ TTS OK: {len(pcm)} bytes PCM, {duration:.2f}s audio", flush=True)
    print(f"  Saved: {out_path}", flush=True)
else:
    print(f"  ❌ TTS failed", flush=True)

print(f"\n{'=' * 50}", flush=True)
print("Done!", flush=True)
