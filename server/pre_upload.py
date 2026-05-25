#!/usr/bin/env python3
"""
Import: PlatformIO extra_script — runs before upload.
Kills any process holding the upload serial port so esptool can access it.
"""
Import("env")

import subprocess
import re


def kill_port_holders():
    """Kill processes using the upload serial port (e.g. usb_bridge.py)."""
    # Get the upload port from environment
    port = env.subst("$UPLOAD_PORT")
    if not port:
        # Fallback: try to detect
        try:
            out = subprocess.check_output(
                ["python3", "-c", "import serial.tools.list_ports; [print(p.device) for p in serial.tools.list_ports.comports() if 'usbmodem' in p.device.lower() or 'usbserial' in p.device.lower()]"],
                stderr=subprocess.DEVNULL, timeout=5
            ).decode().strip()
            if out:
                port = out.split("\n")[0]
        except Exception:
            pass

    if not port:
        print("[pre-upload] No serial port detected, skipping port kill")
        return

    print(f"[pre-upload] Checking if port {port} is in use...")

    # Use lsof to find processes using the port
    try:
        out = subprocess.check_output(
            ["lsof", "-ti", port],
            stderr=subprocess.DEVNULL, timeout=5
        ).decode().strip()
        if out:
            pids = [p.strip() for p in out.split("\n") if p.strip()]
            for pid in pids:
                try:
                    # Get process name for logging
                    pname = subprocess.check_output(
                        ["ps", "-p", pid, "-o", "comm="],
                        stderr=subprocess.DEVNULL, timeout=2
                    ).decode().strip()
                    print(f"[pre-upload] Killing {pname} (PID {pid}) holding {port}")
                    subprocess.run(["kill", pid], timeout=3)
                except Exception as e:
                    print(f"[pre-upload] Failed to kill PID {pid}: {e}")
        else:
            print(f"[pre-upload] Port {port} is free")
    except subprocess.CalledProcessError:
        print(f"[pre-upload] Port {port} is free")
    except Exception as e:
        print(f"[pre-upload] Error checking port: {e}")


kill_port_holders()
