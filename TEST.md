# Test Plan

End-to-end flight recording test. ADXL375 connected via I2C (STEMMA QT: SDA=GPIO5, SCL=GPIO6) with INT1 wired separately to GPIO4 (D3).

## Steps

1. **Build** — `rm -rf build sdkconfig && ./build.sh`; confirm no errors

2. **Flash** — `./flash.sh`

3. **Boot verification** — wait ~12 s, then `./mission-control ping` (expect `pong`); monitor serial log for `DEVID=0xE5 OK`

4. **Status check** — `./mission-control status` (expect `IDLE`)

5. **Flight 1** — `./mission-control trigger`; wait 65 s; `./mission-control pull`; inspect CSV:
   - Correct header: `timestamp_ns,ax_g,ay_g,az_g`
   - ≥46 000 rows (800 Hz × 60 s, allowing minor under-count)
   - Timestamps monotonically increasing at ~1250 us intervals
   - No gap > 62 ms (50 samples × 1250 us)

6. **Flight 2** — repeat trigger → pull → inspect; confirm a new file, same quality criteria

   > After 3–4 flights the SPIFFS partition (~6.4 MB) fills. Run `echo y | ./mission-control wipe` before continuing; wait 3 s after wipe before issuing the next command.

7. **Power cycle** — disconnect and reconnect USB; wait ~12 s for boot; `./mission-control ping`

8. **Post-restart flights** — trigger → pull × 2, same pass criteria. Ensure ≥3 MB free before each flight (`./mission-control df`).

## Pass criteria

- All `mission-control` commands succeed with no `ERROR` lines
- Serial log shows `DEVID=0xE5 OK` on every boot
- All CSVs: correct header, ≥46 000 rows, max gap ≤ 62 ms, monotonic timestamps
