#!/bin/bash
# termux-avrdude
set -e

PTYSERIAL="~/termux-avrdude/bin/ptyserial"
PORT_FILE="$HOME/pty_port.txt"
READY_FILE="$HOME/bridge_ready.txt"
SOCK="$HOME/.ptyserial.sock"
HEX_FILE="$1"
KEEP_BRIDGE=false

# Check for --keep-bridge flag
for arg in "$@"; do
    if [ "$arg" = "--keep-bridge" ]; then
        KEEP_BRIDGE=true
    fi
done

# ==========================================
# PREFLIGHT CHECKS
# ==========================================
if [ -z "$HEX_FILE" ]; then
    echo "Usage: termux-avrdude <hex_file> [--keep-bridge]"
    exit 1
fi

if [ ! -f "$HEX_FILE" ]; then
    echo "Error: hex file not found: $HEX_FILE"
    exit 1
fi

if [ ! -x "$PTYSERIAL" ]; then
    echo "Error: ptyserial not found at $PTYSERIAL"
    exit 1
fi

# ==========================================
# CLEANUP
# ==========================================
BRIDGE_PID=""
cleanup() {
    rm -f "$PORT_FILE" "$READY_FILE"
    if [ "$KEEP_BRIDGE" = false ] && [ -n "$BRIDGE_PID" ]; then
        kill "$BRIDGE_PID" 2>/dev/null || true
        wait "$BRIDGE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ==========================================
# FIND USB DEVICE
# ==========================================
rm -f "$PORT_FILE" "$READY_FILE"

echo "-> Scanning for USB device..."
USB_ADDR=$(termux-usb -l | grep -oE '/dev/bus/usb/[0-9]+/[0-9]+' | head -n 1)
if [ -z "$USB_ADDR" ]; then
    echo "Error: No USB device found"
    exit 1
fi
echo "-> Found device: $USB_ADDR"

# ==========================================
# REQUEST USB PERMISSION FIRST
# Then launch bridge after permission granted
# ==========================================
echo "-> Requesting USB permission (tap Allow if prompted)..."

# Use a temp script as the -e callback
# termux-usb calls this script with the fd as last arg
# We write the fd to a file then ptyserial picks it up
TMPSCRIPT=$(mktemp "$HOME/.termux_usb_XXXXXX.sh")
chmod +x "$TMPSCRIPT"

cat > "$TMPSCRIPT" << INNERSCRIPT
#!/bin/bash
exec "$PTYSERIAL" "\$@"
INNERSCRIPT

termux-usb -r -e "$TMPSCRIPT" "$USB_ADDR" &
BRIDGE_PID=$!

# ==========================================
# WAIT FOR PORT FILE
# ==========================================
echo "-> Waiting for bridge..."
WAITED=0
while [ ! -f "$PORT_FILE" ]; do
    sleep 0.2
    WAITED=$((WAITED + 1))
    if [ $WAITED -gt 150 ]; then
        echo "Error: Timed out waiting for PTY (30s)"
        echo "Make sure you tapped Allow on the USB permission dialog"
        rm -f "$TMPSCRIPT"
        exit 1
    fi
done

rm -f "$TMPSCRIPT"
PTY_PORT=$(tr -d '\r\n ' < "$PORT_FILE")
echo "-> PTY ready: $PTY_PORT"

# ==========================================
# WAIT FOR BRIDGE READY
# ==========================================
WAITED=0
while [ ! -f "$READY_FILE" ]; do
    sleep 0.1
    WAITED=$((WAITED + 1))
    if [ $WAITED -gt 80 ]; then
        echo "Error: Timed out waiting for bridge ready"
        exit 1
    fi
done
echo "-> Bridge ready"

# ==========================================
# SEND RESET
# ==========================================
echo "-> Sending reset..."
echo -n "RESET_AVR" | nc -U "$SOCK" 2>/dev/null || true
sleep 0.2

# ==========================================
# FLASH
# ==========================================
shift # remove hex file, pass remaining args to avrdude
# Also remove --keep-bridge from args
AVRDUDE_ARGS=()
for arg in "$@"; do
    if [ "$arg" != "--keep-bridge" ]; then
        AVRDUDE_ARGS+=("$arg")
    fi
done

echo "-> Flashing $HEX_FILE on $PTY_PORT..."
avrdude \
    -p atmega328p \
    -c arduino \
    -P "$PTY_PORT" \
    -b 115200 \
    -D \
    -U "flash:w:$HEX_FILE:i" \
    "${AVRDUDE_ARGS[@]}"

FLASH_RESULT=$?

if [ $FLASH_RESULT -eq 0 ]; then
    echo "-> Flash successful!"
    if [ "$KEEP_BRIDGE" = true ]; then
        echo "-> Bridge kept alive on $PTY_PORT"
        echo "-> Run: termux-monitor to open serial monitor"
    fi
else
    echo "-> Flash failed (avrdude exit code $FLASH_RESULT)"
fi

exit $FLASH_RESULT
