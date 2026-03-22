# I2C Implementation Notes (ESP32-S3 + ADXL375)

Learnings from the ADXL375 integration. Useful reference if I2C is used again.

## Physical connection

- SDA = GPIO5 (D4), SCL = GPIO6 (D5) wired to ADXL375 breakout board pins
- Internal ESP32-S3 pull-ups (~45 kΩ) work reliably at 400 kHz / 3.3 V for cable lengths ≤ 10 cm
- External 4.7 kΩ pull-ups required at longer cable lengths

## ESP-IDF driver (new-style)

Uses `driver/i2c_master.h` (not the legacy `driver/i2c.h`):

- Handle types: `i2c_master_bus_handle_t` + `i2c_master_dev_handle_t`
- Bus config: `I2C_NUM_0`, `I2C_CLK_SRC_DEFAULT`, `glitch_ignore_cnt=7`, internal pull-ups enabled
- Device config: 400 kHz, 100 ms timeout, 3 retries per transaction
- Teardown order: `i2c_master_bus_rm_device()` before `i2c_del_master_bus()`

## ADXL375 address

- Default: 0x53 (SDO/CS low)
- Alternate: 0x1D (SDO/CS high)
- `adxl375_init()` probes both addresses

## Burst read

One `i2c_master_transmit_receive` call per sample: write register address `0x32`, read 6 bytes (DATAX0–DATAZ1). For SPI migration: same register addresses, different framing.

## Soft-reset recovery

After an ESP32 button reset (without power-cycling the ADXL375), the sensor keeps power and may hold SDA low mid-transaction. Recovery sequence in `adxl375_init()`:

1. Call `i2c_master_bus_reset()`
2. Wait ≥ 50 ms
3. Re-probe DEVID register (expect 0xE5)

If the probe still fails, `main.c` calls `adxl375_reinit()` every 5 s for up to 5 minutes. `adxl375_reinit()` tears down both handles and calls `adxl375_init()` from scratch. In practice one retry (~5 s) is sufficient.

## Bus scan

`i2c_scan()` probes addresses 0x08–0x77 and logs results to serial. Called automatically on probe failure to aid wiring diagnosis.
