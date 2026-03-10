# Architecture

ESP-IDF v5.4 application running two FreeRTOS tasks on an ESP32-S3.

## Tasks

| Task               | Core       | Priority | Stack | Role                                         |
|--------------------|------------|----------|-------|----------------------------------------------|
| `flight_task`      | 1 (pinned) | 5        | 4096  | Sensor polling, state machine, launch detect |
| `log_write_task`   | 0 (pinned) | 5        | 4096  | SPIFFS writes (decoupled from FIFO reads)    |
| `serial_cmd_task`  | any        | 3        | 4096  | Serial command handler (stdin/stdout)        |

`flight_task` and `log_write_task` only run in flight mode. `serial_cmd_task` runs in both modes.

## Modules

```
main.c            Boot sequence, GPIO mode select, task creation
adxl375.c/.h      I2C driver for ADXL375 (400kHz bus, register-level)
flight_logger.c/.h  State machine + ring buffer + CSV recording
storage.c/.h      SPIFFS mount, flight file lifecycle, CSV writing
serial_cmd.c/.h   Command dispatch, response framing
led.c/.h          LEDC PWM patterns (breathe, flash, transfer, blink)
```

## State machine

```
IDLE --(|a| > 3g for 50ms)--> LOGGING --(60s)--> COOLDOWN --(3s)--> IDLE
  ^                                                                    |
  |                  "transfer" cmd (from any IDLE state)             |
  +----------- "resume" cmd or 30s timeout <-- TRANSFER <------------+
```

## Flight recording pipeline

1. `flight_task` (Core 1) drains ADXL375 FIFO (up to 32 samples/read via I2C burst)
2. In IDLE: samples go into a 800-entry pre-trigger ring buffer (2s at 400 Hz)
3. On launch detect: pre-trigger buffer + live samples are pushed into a 4000-entry RAM ring buffer (`s_log_ring`)
4. `log_write_task` (Core 0) drains `s_log_ring` → SPIFFS. Flash erase stalls only Core 0; `flight_task` keeps reading the FIFO uninterrupted
5. After 60s: `flight_task` signals flush, `log_write_task` closes the file, 3s cooldown, return to IDLE

## Serial protocol

Bidirectional over USB Serial/JTAG controller (not USB-OTG). Requires `usb_serial_jtag_driver_install()` + `usb_serial_jtag_vfs_use_driver()` for `printf`/`getchar` to work.

- Boot marker: `FORCE4:READY\n` (printed at startup for diagnostics; data.sh does not wait for it)
- Response framing: `---BEGIN---\n` ... `---END---\n` around every command response
- Commands: `ls`, `cat <file>`, `rm <file>`, `status`, `transfer`, `resume`, `ping`, `help`

## Partition layout

| Name     | Type   | Offset   | Size   |
|----------|--------|----------|--------|
| nvs      | data   | 0x9000   | 24K    |
| phy_init | data   | 0xF000   | 4K     |
| factory  | app    | 0x10000  | 1.5M   |
| storage  | SPIFFS | 0x190000 | ~6.4M  |

## ADXL375 configuration

| Register            | Value | Purpose                          |
|---------------------|-------|----------------------------------|
| BW_RATE (0x2C)      | 0x0C  | 400 Hz ODR                       |
| POWER_CTL (0x2D)    | 0x08  | Measurement mode                 |
| DATA_FORMAT (0x31)  | 0x0B  | Full resolution, right-justified |
| FIFO_CTL (0x38)     | 0x90  | Stream mode, watermark=16        |

Scale: 49 mg/LSB (raw * 0.049 = g). Timestamps estimated backward from read time at 2500us intervals.

### Soft-reset recovery

After an ESP32 button reset (not power-cycle), the ADXL375 keeps power but may be stuck mid-transaction, causing the I2C probe to fail. `adxl375_init()` issues `i2c_master_bus_reset()` and waits 50ms before probing to allow the sensor to recover.

If the probe still fails, `main.c` retries every 5s for up to 5 minutes via `adxl375_reinit()`, which tears down the I2C bus and device handles and calls `adxl375_init()` from scratch. In practice one retry (≈5s) is sufficient.
