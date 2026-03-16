#!/bin/bash
set -euo pipefail

PORT_FILE="/data/data/com.termux/files/home/pty_port.txt"
HEX_FILE="blink.hex"
READY_FILE="/data/data/com.termux/files/home/bridge_ready.txt"
BAUD=115200
MCU=atmega328p

# Cleanup from any previous run
rm -f "$PORT_FILE" "$READY_FILE"

# Find USB device
USB_ADDR=$(termux-usb -l | grep -oE '/dev/bus/usb/[0-9]+/[0-9]+' | head -n 1)
if [ -z "$USB_ADDR" ]; then
    echo "Error: No USB device found."
    exit 1
fi
echo "-> Found Arduino at $USB_ADDR"

# Watcher runs in background
(
    set -x
    # Step 1: wait for port file to appear
    echo "-> Waiting for bridge..."
    WAIT=0
    while [ ! -f "$PORT_FILE" ]; do
        sleep 0.1
        WAIT=$((WAIT+1))
        if [ $WAIT -gt 100 ]; then
            echo "Error: Timed out waiting for PTY port."
            pkill -f ptyserial 2>/dev/null
            exit 1
        fi
    done

    PTY_PORT=$(cat "$PORT_FILE" | tr -d '\r\n ')
    echo "-> PTY port: $PTY_PORT"

    # Step 2: wait for ready file (written by ptyserial AFTER DTR reset)
    echo "-> Waiting for bridge ready signal..."
    WAIT=0
    while [ ! -f "$READY_FILE" ]; do
        sleep 0.1
        WAIT=$((WAIT+1))
        if [ $WAIT -gt 50 ]; then
            echo "Error: Timed out waiting for bridge ready."
            pkill -f ptyserial 2>/dev/null
            exit 1
        fi
    done

    # Step 3: wait for Arduino bootloader window
    # DTR reset takes ~100ms, bootloader lives for~ 1.5s
    # We want to hit it roughly 200ms after reset
    sleep 0.2

    echo "-> Firing avrdude on $PTY_PORT..."
    if avrdude -v -p $MCU -c arduino -P "$PTY_PORT" -b $BAUD -D -U flash:w:"$HEX_FILE":i; then
        echo "-> Flash successful!"
    else
        echo "-> Flash FAILED. Try adjusting sleep before avrdude."
    fi

    echo "-> Cleaning up..."
    rm -f "$PORT_FILE" "$READY_FILE"
    pkill -f ptyserial 2>/dev/null || true
    echo "-> Done!"
) &

WATCHER_PID=$!

echo "-> Starting bridge..."
termux-usb -r -e "./bin/ptyserial" "$USB_ADDR"

# If bridge exits unexpectedly, kill watcher too
kill $WATCHER_PID 2>/dev/null || true
