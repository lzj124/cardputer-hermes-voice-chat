#!/usr/bin/env python3
"""Wrapper for usb_bridge that forces unbuffered output."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'server'))

# Force unbuffered
sys.stdout.reconfigure(line_buffering=True) if hasattr(sys.stdout, 'reconfigure') else None
os.environ['PYTHONUNBUFFERED'] = '1'

from usb_bridge import main
main()
