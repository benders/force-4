# Test Plan: SPI Refactor End-to-End

Autonomous agent test procedure after rewiring ADXL375 to SPI (see wiring in this commit's message or `ARCHITECTURE.md`).

## Steps

1. **Build** — `./build.sh`; confirm output contains no `i2c_*` symbols

2. **Flash** — `./flash.sh`

3. **Boot verification** — `./mission-control ping` (expect `pong`); monitor serial log for `DEVID=0xE5 OK`

4. **Status check** — `./mission-control status` (expect `IDLE`)

5. **Flight 1** — `./mission-control trigger`; wait 65 s; `./mission-control pull`; inspect CSV:
   - Correct header: `timestamp_ns,ax_g,ay_g,az_g`
   - ≥23 000 rows (400 Hz × 60 s, allowing minor under-count)
   - Timestamps monotonically increasing at ~2500 µs intervals
   - No gap > 125 ms (50 samples × 2500 µs)

6. **Flight 2** — repeat trigger → pull → inspect; confirm a new file, same quality criteria

7. **Flight 3** — repeat again

8. **Power cycle** — disconnect and reconnect USB; wait for boot; `./mission-control ping`

9. **Post-restart flights** — trigger → pull × 2, same pass criteria

## Pass criteria

- All `mission-control` commands succeed with no `ERROR` lines
- Serial log shows `DEVID=0xE5 OK` on every boot
- All CSVs: correct header, ≥23 000 rows, no timestamp gap > 125 ms
