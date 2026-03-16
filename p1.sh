#!/bin/bash
SOCK="$HOME/.ptyserial.sock"

# Check bridge is running
if [ ! -S "$SOCK" ]; then
    echo "Error: ptyserial bridge not running"
    exit 1
fi

# Send AVR reset sequence
echo -n "RESET_AVR" | nc -U "$SOCK"

# Small gap — avrdude connects during bootloader window
sleep 0.5

# Pass all arguments straight to real avrdude
exec avrdude "$@"
