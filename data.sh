#!/usr/bin/env bash
# Force-4 serial data management
# Usage: ./data.sh <verb> [args]
# Verbs: pull [file], df, wipe [file]
set -e

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "ERROR: No USB modem device found. Is the board connected?"
  exit 1
fi

# Send a command and capture output.
# Uses python for reliable serial I/O with timeouts.
send_cmd() {
  python3 -c "
import serial, sys, time
cmd = sys.argv[1]
port = sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0

ser = serial.Serial(port, 115200, timeout=0.1)
time.sleep(0.1)
# Flush any pending output
ser.read(4096)

# Send command
ser.write((cmd + '\n').encode())
ser.flush()

# Read response until timeout with no new data
deadline = time.time() + timeout
last_data = time.time()
buf = b''
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        last_data = time.time()
        deadline = max(deadline, last_data + 0.5)
    elif time.time() - last_data > 0.3:
        break
    time.sleep(0.01)

ser.close()
sys.stdout.buffer.write(buf)
" "$@"
}

case "${1:-}" in
  pull)
    if [ -n "${2:-}" ]; then
      FILE="$2"
    else
      # Get most recent flight file
      FILE=$(send_cmd "ls" "$PORT" 2 | grep 'flight_' | tail -1 | awk '{print $1}' | tr -d '\r')
    fi
    if [ -z "$FILE" ]; then
      echo "No flight data on device."
      exit 1
    fi
    echo "Downloading $FILE ..."
    send_cmd "cat $FILE" "$PORT" 30 > "$FILE"
    SIZE=$(wc -c < "$FILE" | tr -d ' ')
    echo "Saved $FILE ($SIZE bytes)"
    ;;

  df)
    send_cmd "status" "$PORT" 2
    echo "---"
    send_cmd "ls" "$PORT" 2
    ;;

  wipe)
    if [ -n "${2:-}" ]; then
      FILE="$2"
      read -p "Delete $FILE from device? [y/N] " confirm
      if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "Aborted."
        exit 0
      fi
      send_cmd "rm $FILE" "$PORT" 2
    else
      echo "Listing files..."
      FILES=$(send_cmd "ls" "$PORT" 2 | grep 'flight_' | awk '{print $1}' | tr -d '\r')
      if [ -z "$FILES" ]; then
        echo "No files to delete."
        exit 0
      fi
      echo "$FILES"
      read -p "Delete ALL listed files? [y/N] " confirm
      if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "Aborted."
        exit 0
      fi
      for f in $FILES; do
        send_cmd "rm $f" "$PORT" 2
        echo "Deleted $f"
      done
    fi
    ;;

  *)
    echo "Usage: $0 <verb>"
    echo ""
    echo "  pull [file]   Download a flight CSV (default: most recent)"
    echo "  df            Show storage usage and list flights"
    echo "  wipe [file]   Delete flight data (default: all)"
    exit 1
    ;;
esac
