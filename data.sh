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
# The board is already running when we open the port — no DTR reset occurs
# on XIAO ESP32-S3 over USB Serial/JTAG.
#
# The firmware frames command responses with ---BEGIN--- / ---END--- markers.
#
# Before sending the real command, we send "transfer" to pause flight logging
# and activate the data-transfer LED pattern (double-blink).
send_cmd() {
  python3 -c "
import serial, sys, time

cmd = sys.argv[1]
port = sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0

ser = serial.Serial(port, 115200, timeout=0.1)

# Board is already running; wait briefly and discard any pending output.
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

# Send resume to return device to IDLE (flight ready) state
ser.write(b'resume\r\n')
ser.flush()
resume_buf = b''
resume_deadline = time.time() + 2.0
while time.time() < resume_deadline:
    chunk = ser.read(4096)
    if chunk:
        resume_buf += chunk
        if b'---END---' in resume_buf:
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

# Send a single command with no transfer/resume — does not change device state.
query_state() {
  python3 -c "
import serial, sys, time

port    = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0

ser = serial.Serial(port, 115200, timeout=0.1)
time.sleep(0.2)
ser.reset_input_buffer()

ser.write(b'status\r\n')
ser.flush()

buf      = b''
deadline = time.time() + timeout
last_rx  = time.time()
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf     += chunk
        last_rx  = time.time()
        if b'---END---' in buf:
            break
        deadline = max(deadline, last_rx + 0.5)
    elif time.time() - last_rx > 0.8:
        break
    time.sleep(0.01)

ser.close()

text  = buf.decode(errors='replace')
begin = text.find('---BEGIN---')
end   = text.find('---END---')
if begin >= 0:
    content = text[begin + 11:end if end >= 0 else len(text)]
    sys.stdout.write(content.strip() + '\n')
elif text.strip():
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
      # Get most recent non-empty flight file.
      # ls output is "filename  size"; sort by name (zero-padded = numeric order),
      # filter out zero-length files (the pre-opened ready file), take the last.
      FILE=$(send_cmd "ls" "$PORT" 4 | grep 'flight_' | awk '$2 > 0 {print $1}' | sort | tail -1 | tr -d '\r')
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
      RESULT=$(send_cmd "rm $FILE" "$PORT" 4)
      if echo "$RESULT" | grep -qi "denied\|error"; then
        echo "FAILED: $RESULT"
        exit 1
      else
        echo "Deleted $FILE"
      fi
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
      # Delete all files in a single serial session to avoid reconnect issues.
      python3 -c "
import serial, sys, time

files = sys.argv[1].split()
port  = sys.argv[2]

def read_response(ser, timeout=4.0):
    buf = b''
    deadline = time.time() + timeout
    last_rx   = time.time()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf     += chunk
            last_rx  = time.time()
            if b'---END---' in buf:
                break
            deadline = max(deadline, last_rx + 0.5)
        elif time.time() - last_rx > 0.8:
            break
        time.sleep(0.01)
    text  = buf.decode(errors='replace')
    begin = text.find('---BEGIN---')
    end   = text.find('---END---')
    if begin >= 0:
        return text[begin + 11 : end if end >= 0 else len(text)].strip()
    return text.strip()

ser = serial.Serial(port, 115200, timeout=0.1)
time.sleep(0.2)
ser.reset_input_buffer()

ser.write(b'transfer\r\n')
ser.flush()
read_response(ser, timeout=3.0)
time.sleep(0.1)
ser.reset_input_buffer()

ok = True
for f in files:
    ser.write(('rm ' + f + '\r\n').encode())
    ser.flush()
    result = read_response(ser)
    if 'denied' in result.lower() or 'error' in result.lower():
        print('FAILED {}: {}'.format(f, result))
        ok = False
    else:
        print('Deleted {}'.format(f))
    time.sleep(0.1)
    ser.reset_input_buffer()

# Return device to IDLE (flight ready) state
ser.write(b'resume\r\n')
ser.flush()
resume_buf = b''
resume_deadline = time.time() + 2.0
while time.time() < resume_deadline:
    chunk = ser.read(4096)
    if chunk:
        resume_buf += chunk
        if b'---END---' in resume_buf:
            break
    time.sleep(0.01)

ser.close()
sys.exit(0 if ok else 1)
" "$FILES" "$PORT"
    fi
    ;;

  trigger)
    python3 -c "
import serial, sys, time

port    = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0

ser = serial.Serial(port, 115200, timeout=0.1)
time.sleep(0.2)
ser.reset_input_buffer()

ser.write(b'trigger\r\n')
ser.flush()

buf      = b''
deadline = time.time() + timeout
last_rx  = time.time()
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf     += chunk
        last_rx  = time.time()
        if b'---END---' in buf:
            break
        deadline = max(deadline, last_rx + 0.5)
    elif time.time() - last_rx > 0.8:
        break
    time.sleep(0.01)

ser.close()

text  = buf.decode(errors='replace')
begin = text.find('---BEGIN---')
end   = text.find('---END---')
if begin >= 0:
    content = text[begin + 11:end if end >= 0 else len(text)].strip()
    if 'denied' in content:
        sys.stderr.write('Error: ' + content + '\n')
        sys.exit(1)
    sys.stdout.write(content + '\n')
elif text.strip():
    sys.stderr.write('Warning: no response markers found\n')
    sys.stdout.write(text.strip() + '\n')
else:
    sys.stderr.write('Error: no response from device\n')
    sys.exit(1)
" "$PORT" 4
    ;;

  state)
    query_state "$PORT" 4
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
    echo "  trigger       Manually start a flight recording (device must be IDLE)"
    echo "  state         Show device state without changing it"
    echo "  diag          Run device health diagnostics"
    exit 1
    ;;
esac
