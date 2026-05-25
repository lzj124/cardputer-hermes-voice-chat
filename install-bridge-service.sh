#!/bin/bash
# Install ClawVoice USB Bridge as a macOS launchd service
# This makes usb_bridge.py start automatically on boot and when ESP32 is plugged in.

PLIST="com.clawvoice.usb-bridge.plist"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Installing ClawVoice USB Bridge launchd service..."
echo "  Source: $SRC_DIR/$PLIST"

# Copy plist to LaunchAgents
cp "$SRC_DIR/$PLIST" "$HOME/Library/LaunchAgents/$PLIST"

# Load the service
launchctl load "$HOME/Library/LaunchAgents/$PLIST"

echo ""
echo "✅ Installed! The bridge will start on boot."
echo "   To check status: launchctl list com.clawvoice.usb-bridge"
echo "   To see logs:     tail -f ~/Library/Logs/clawvoice-bridge.log"
echo "   To stop:         launchctl unload ~/Library/LaunchAgents/$PLIST"
echo "   To restart:      launchctl kickstart gui/$(id -u)/com.clawvoice.usb-bridge"
