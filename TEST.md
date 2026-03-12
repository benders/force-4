# Test Plan: SPI Refactor End-to-End

Autonomous agent test procedure after rewiring ADXL375 to SPI (see wiring in this commit's message or `ARCHITECTURE.md`).

## Steps

1. **Build** — `./build.sh`; confirm output contains no `i2c_*` symbols

2. **Flash** — `./flash.sh`

3. **Boot verification** — wait ~12 s, then `./mission-control ping` (expect `pong`); monitor serial log for `DEVID=0xE5 OK`

4. **Status check** — `./mission-control status` (expect `IDLE`)

5. **Flight 1** — `./mission-control trigger`; wait 65 s; `./mission-control pull`; inspect CSV:
   - Correct header: `timestamp_ns,ax_g,ay_g,az_g`
   - ≥46 000 rows (800 Hz × 60 s, allowing minor under-count). Filter log lines: `[r for r in csv.DictReader(f) if r['timestamp_ns'].strip().isdigit()]`
   - Timestamps monotonically increasing at ~1250 µs intervals
   - No gap > 62 ms (50 samples × 1250 µs)

6. **Flight 2** — repeat trigger → pull → inspect; confirm a new file, same quality criteria

7. **Flight 3** — repeat again

   > After 3–4 flights the SPIFFS partition (~6.4 MB) fills. Run `echo y | ./mission-control wipe` before continuing; wait 3 s after wipe before issuing the next command.

8. **Power cycle** — disconnect and reconnect USB; wait ~12 s for boot; `./mission-control ping`

9. **Post-restart flights** — trigger → pull × 2, same pass criteria. Ensure ≥3 MB free before each flight (`./mission-control df`).

## Pass criteria

- All `mission-control` commands succeed with no `ERROR` lines
- Serial log shows `DEVID=0xE5 OK` on every boot
- All CSVs: correct header, ≥46 000 rows, no timestamp gap > 62 ms
