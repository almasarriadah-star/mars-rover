#!/bin/bash
# setup_bluetooth.sh — Bind JDY-31-SPP to /dev/rfcomm0
# Uses passwordless sudo (configured in /etc/sudoers.d/bluetooth)

DEVICE_ADDR="78:87:13:04:09:75"
DEVICE_NAME="JDY-31-SPP"
RFCOMM_PORT=0
SERIAL_PORT="/dev/rfcomm0"

echo "🔧 Setting up Bluetooth connection to $DEVICE_NAME ($DEVICE_ADDR)..."

# Release any existing binding
sudo rfcomm release $RFCOMM_PORT 2>/dev/null

# Scan and wait for the device to appear
echo "📡 Scanning for devices (10 sec)..."
sudo hcitool scan > /tmp/bt_scan.txt 2>&1
sleep 2
sudo hcitool scan >> /tmp/bt_scan.txt 2>&1

if grep -q "$DEVICE_ADDR" /tmp/bt_scan.txt; then
    echo "   ✅ Device found!"
else
    echo "   ⚠️ Device not found in scan. Make sure it's powered on."
fi

# Trust the device
echo "🤝 Trusting device..."
sudo bluetoothctl trust $DEVICE_ADDR 2>/dev/null

# Bind RFCOMM with sudo
echo "🔗 Binding RFCOMM port $RFCOMM_PORT..."
sudo rfcomm bind $RFCOMM_PORT $DEVICE_ADDR 1

if [ -e "$SERIAL_PORT" ]; then
    # Set permissions so Python can read/write without sudo
    sudo chmod 666 $SERIAL_PORT
    echo "✅ Success! $SERIAL_PORT is ready."
    echo "   Device: $DEVICE_NAME"
    echo "   Address: $DEVICE_ADDR"
    echo "   Baudrate: 9600"
    echo ""
    # Quick test: try to read something
    echo "📡 Testing connection..."
    timeout 2 cat $SERIAL_PORT 2>/dev/null && echo "" || echo "   (no data yet — normal if Arduino is off)"
else
    echo "❌ Failed to create $SERIAL_PORT"
    echo "   Make sure device is powered on and in range"
    exit 1
fi
