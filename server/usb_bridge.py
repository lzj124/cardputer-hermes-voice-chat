#!/usr/bin/env python3
"""
ClawVoice USB Bridge — Serial ↔ proxy.py HTTP bridge

Usage:
    python3 usb_bridge.py [serial_port] [proxy_url]

    serial_port: defaults to auto-detect (Cardputer/StampS3)
    proxy_url:   defaults to http://127.0.0.1:8900

Protocol:
    ESP32 → "HELLO\n"          → bridge responds "OK\n"
    ESP32 → "SEND <len>\n<pcm> → bridge POSTs to proxy /voice, responds "RECV <len>\n<pcm>" or "ERR\n"
    Other serial lines (debug) are printed to console with [ESP] prefix.
"""

import sys
import time
import serial
import serial.tools.list_ports
import requests

PROXY_URL = "http://127.0.0.1:8900"


def find_port():
    """Auto-detect Cardputer USB serial port."""
    for p in serial.tools.list_ports.comports():
        # ESP32-S3 shows as "USB JTAG/serial debug unit" or similar
        if "usbmodem" in p.device.lower() or "usbserial" in p.device.lower():
            return p.device
    return None


def read_line(ser, timeout=5):
    """Read a line from serial, return string without newline."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        b = ser.read(1)
        if b == b"\n":
            return buf.decode("utf-8", errors="replace").rstrip("\r")
        if b:
            buf += b
        else:
            time.sleep(0.01)
    return None


def read_exact(ser, n, timeout=120):
    """Read exactly n bytes from serial."""
    deadline = time.time() + timeout
    data = b""
    while len(data) < n and time.time() < deadline:
        chunk = ser.read(n - len(data))
        if chunk:
            data += chunk
        else:
            time.sleep(0.01)
    return data


def open_serial(port):
    """Open serial port, return serial object or None."""
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
        time.sleep(0.5)
        return ser
    except Exception as e:
        print(f"[BRIDGE] Failed to open {port}: {e}")
        return None


def do_handshake(ser):
    """Wait for HELLO or SEND. If SEND, process it and enter relay. Returns True on success."""
    print("[BRIDGE] Waiting for ESP32 handshake...")
    buf = b""
    deadline = time.time() + 30  # wait up to 30s

    while time.time() < deadline:
        try:
            b = ser.read(1)
        except serial.SerialException:
            print("[BRIDGE] Serial disconnected during handshake")
            return False

        if b == b"\n":
            line = buf.decode("utf-8", errors="replace").rstrip("\r")
            buf = b""
            if line == "HELLO":
                print("[BRIDGE] Got HELLO, sending OK")
                ser.write(b"OK\n")
                ser.flush()
                return True
            elif line.startswith("SEND "):
                # ESP32 already in USB mode from previous session — process directly
                print(f"[BRIDGE] Got SEND without handshake, processing...")
                pcm_len = int(line[5:])
                return _process_send(ser, pcm_len)
            else:
                print(f"[ESP] {line}")
        elif b:
            buf += b
        else:
            time.sleep(0.01)

    print("[BRIDGE] Handshake timeout")
    return False


def _process_send(ser, pcm_len):
    """Handle a SEND command: read PCM, forward to proxy, send back RECV+PCM.
    Returns True to enter relay loop, False on error."""
    print(f"[BRIDGE] Receiving {pcm_len} bytes PCM from ESP32...")

    pcm_data = read_exact(ser, pcm_len)
    if len(pcm_data) != pcm_len:
        print(f"[BRIDGE] Short read: got {len(pcm_data)}/{pcm_len}")
        try:
            ser.write(b"ERR\n")
            ser.flush()
        except serial.SerialException:
            return False
        return True

    print(f"[BRIDGE] Forwarding to proxy /voice...")
    try:
        resp = requests.post(
            f"{PROXY_URL}/voice",
            data=pcm_data,
            headers={"Content-Type": "application/pcm"},
            timeout=120,
            stream=True
        )
        if resp.status_code == 200:
            pcm_data = resp.content
            if pcm_data:
                ser.write(f"RECV {len(pcm_data)}\n".encode())
                ser.flush()
                time.sleep(0.1)
                sent = 0
                while sent < len(pcm_data):
                    c = pcm_data[sent:sent+128]
                    ser.write(c)
                    ser.flush()
                    sent += len(c)
                    time.sleep(0.001)
                print(f"[BRIDGE] TTS relay complete ({len(pcm_data)} bytes)")
            else:
                ser.write(b"RECV 0\n")
                ser.flush()
        else:
            print(f"[BRIDGE] Proxy error: {resp.status_code} {resp.text[:200]}")
            ser.write(b"ERR\n")
            ser.flush()
    except Exception as e:
        print(f"[BRIDGE] Proxy request failed: {e}")
        try:
            ser.write(b"ERR\n")
            ser.flush()
        except serial.SerialException:
            return False
    return True


def handle_relay(ser):
    """Main relay loop. Returns when serial disconnects."""
    buf = b""
    while True:
        try:
            b = ser.read(1)
        except serial.SerialException:
            print("[BRIDGE] Serial disconnected")
            return

        if not b:
            continue

        if b == b"\n":
            line = buf.decode("utf-8", errors="replace").rstrip("\r")
            buf = b""

            if line == "HELLO":
                # Re-handshake — ESP32 might have reset
                print("[BRIDGE] Got HELLO (re-handshake), sending OK")
                ser.write(b"OK\n")
                ser.flush()
            elif line.startswith("SEND "):
                pcm_len = int(line[5:])
                _process_send(ser, pcm_len)
            else:
                # Debug output from ESP32
                print(f"[ESP] {line}")
        else:
            buf += b


def main():
    global PROXY_URL

    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if len(sys.argv) > 2:
        PROXY_URL = sys.argv[2]

    if not port:
        print("[BRIDGE] No serial port found. Connect Cardputer via USB.")
        sys.exit(1)

    print(f"[BRIDGE] Using {port}, proxy={PROXY_URL}")

    # Outer loop: auto-reconnect on serial disconnect
    while True:
        ser = open_serial(port)
        if not ser:
            print("[BRIDGE] Retrying in 3s...")
            time.sleep(3)
            port = find_port() or port  # re-detect
            continue

        if do_handshake(ser):
            print("[BRIDGE] Handshake complete. Ready to relay.")
            handle_relay(ser)

        # If we get here, serial disconnected or handshake failed
        try:
            ser.close()
        except:
            pass
        print("[BRIDGE] Reconnecting in 2s...")
        time.sleep(2)
        port = find_port() or port


if __name__ == "__main__":
    main()
