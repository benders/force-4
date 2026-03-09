#!/usr/bin/env bash
# Force-4 serial data management
# Usage: ./data.sh <verb> [args]
# Verbs: pull [file], df, wipe [file], diag
set -e

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "ERROR: No USB modem device found. Is the board connected?"
  exit 1
fi

# Send a command over serial and capture the response.
#
# The XIAO ESP32-S3 has a hardware auto-reset circuit (DTR -> RESET via RC)
# that reboots the board whenever the serial port is opened.
# We accept this: wait for the board to finish booting, then send the command.
#
# The firmware prints "FORCE4:READY" when the serial command handler is ready,
# and frames command responses with ---BEGIN--- / ---END--- markers.
#
# Before sending the real command, we send "transfer" to pause flight logging
# and activate the data-transfer LED pattern (double-blink).
#
# Note: this means every data.sh invocation reboots the board and interrupts
# any active flight recording. Use data.sh only after landing.
send_cmd() {
  python3 -c "
import serial, sys, time

cmd = sys.argv[1]
port = sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0

ser = serial.Serial(port, 115200, timeout=0.1)

# Opening the port triggers a hardware reset via DTR on XIAO ESP32-S3.
# Wait for FORCE4:READY marker (serial_cmd_task is ready for input).
boot_buf = b''
boot_deadline = time.time() + 8.0
ready = False
while time.time() < boot_deadline:
    chunk = ser.read(4096)
    if chunk:
        boot_buf += chunk
        if b'FORCE4:READY' in boot_buf:
            ready = True
            break
    time.sleep(0.01)

if not ready:
    # Fallback: wait for silence after receiving some boot output
    if boot_buf:
        time.sleep(1.0)
    else:
        sys.stderr.write('Warning: no boot output received\n')
        time.sleep(3.0)

# Give the getchar() loop a moment to start, then clear stale data
time.sleep(0.2)
ser.reset_input_buffer()

# Enter transfer state: pauses flight logging, activates double-blink LED
ser.write(b'transfer\r\n')
ser.flush()
xfer_buf = b''
xfer_deadline = time.time() + 3.0
while time.time() < xfer_deadline:
    chunk = ser.read(4096)
    if chunk:
        xfer_buf += chunk
        if b'---END---' in xfer_buf:
            break
    time.sleep(0.01)

# Clear any residual before sending the real command
time.sleep(0.1)
ser.reset_input_buffer()

# Send command with CR+LF (compatible with all terminal modes)
ser.write((cmd + '\r\n').encode())
ser.flush()

# Collect response until ---END--- marker or timeout
buf = b''
deadline = time.time() + timeout
last_rx = time.time()
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        last_rx = time.time()
        if b'---END---' in buf:
            break
        deadline = max(deadline, last_rx + 0.5)
    elif time.time() - last_rx > 0.8:
        break
    time.sleep(0.01)

ser.close()

# Extract content between ---BEGIN--- and ---END--- markers
text = buf.decode(errors='replace')
begin = text.find('---BEGIN---')
end = text.find('---END---')
if begin >= 0:
    content = text[begin + 11:end if end >= 0 else len(text)]
    sys.stdout.write(content.strip() + '\n')
elif text.strip():
    # No markers found — print raw (might be ESP_LOGI or error)
    sys.stderr.write('Warning: no response markers found\n')
    sys.stdout.write(text.strip() + '\n')
else:
    sys.stderr.write('Error: no response from device\n')
    sys.exit(1)
" "$@"
}

case "${1:-}" in
  pull)
    if [ -n "${2:-}" ]; then
      FILE="$2"
    else
      # Get most recent flight file
      FILE=$(send_cmd "ls" "$PORT" 4 | grep 'flight_' | tail -1 | awk '{print $1}' | tr -d '\r')
    fi
    if [ -z "$FILE" ]; then
      echo "No flight data on device."
      exit 1
    fi
    echo "Downloading $FILE ..."
    # Use framing extraction (send_cmd) — strips log lines and BEGIN/END markers,
    # leaving only the clean CSV content.
    send_cmd "cat $FILE" "$PORT" 60 > "$FILE"
    SIZE=$(wc -c < "$FILE" | tr -d ' ')
    echo "Saved $FILE ($SIZE bytes)"
    ;;

  df)
    send_cmd "status" "$PORT" 4
    echo "---"
    send_cmd "ls" "$PORT" 4
    ;;

  wipe)
    if [ -n "${2:-}" ]; then
      FILE="$2"
      read -p "Delete $FILE from device? [y/N] " confirm
      if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "Aborted."
        exit 0
      fi
      send_cmd "rm $FILE" "$PORT" 4
    else
      echo "Listing files..."
      FILES=$(send_cmd "ls" "$PORT" 4 | grep 'flight_' | awk '{print $1}' | tr -d '\r')
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
        send_cmd "rm $f" "$PORT" 4
        echo "Deleted $f"
      done
    fi
    ;;

  diag)
    echo "=== Force-4 Diagnostics ==="
    echo "Port: $PORT"
    echo ""

    echo "--- Ping ---"
    PING=$(send_cmd "ping" "$PORT" 4 2>&1) || true
    if echo "$PING" | grep -q "pong"; then
      echo "OK: device responded to ping"
    else
      echo "FAIL: no pong response"
      echo "  Got: $PING"
      echo ""
      echo "Troubleshooting:"
      echo "  1. Is the board powered? (check LED)"
      echo "  2. Try: stty -f $PORT 115200 raw && cat $PORT"
      echo "     (should show boot messages within 5s)"
      echo "  3. If no output, try unplugging and replugging USB"
      exit 1
    fi
    echo ""

    echo "--- Status ---"
    send_cmd "status" "$PORT" 4 || echo "FAIL: status command failed"
    echo ""

    echo "--- Storage ---"
    send_cmd "ls" "$PORT" 4 || echo "FAIL: ls command failed"
    echo ""

    echo "=== All checks passed ==="
    ;;

  *)
    echo "Usage: $0 <verb>"
    echo ""
    echo "  pull [file]   Download a flight CSV (default: most recent)"
    echo "  df            Show storage usage and list flights"
    echo "  wipe [file]   Delete flight data (default: all)"
    echo "  diag          Run device health diagnostics"
    exit 1
    ;;
esac
