# Architecture

ESP-IDF v5.4 application running two FreeRTOS tasks on an ESP32-S3.

## Tasks

| Task               | Core       | Priority | Stack | Role                                        |
|--------------------|------------|----------|-------|---------------------------------------------|
| `flight_task`      | 1 (pinned) | 5        | 4096  | Sensor polling, state machine, file writing  |
| `serial_cmd_task`  | any        | 3        | 4096  | Serial command handler (stdin/stdout)        |

`flight_task` only runs in flight mode. `serial_cmd_task` runs in both modes.

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
IDLE --(|a| > 3g for 50ms)--> LOGGING --(60s)--> COOLDOWN --(10s)--> IDLE
                                                      \
TRANSFER <--(serial "transfer" cmd)                    any state
```

## Flight recording pipeline

1. `flight_task` drains ADXL375 FIFO (up to 32 samples/read via I2C burst)
2. In IDLE: samples go into a ring buffer (800 entries = 2s at 400 Hz)
3. On launch detect: ring buffer drains to new CSV file, then live samples accumulate in a write buffer
4. Write buffer flushes to SPIFFS every 256 samples
5. After 60s: file closed, 10s cooldown, return to IDLE

## Serial protocol

Bidirectional over USB Serial/JTAG controller (not USB-OTG). Requires `usb_serial_jtag_driver_install()` + `usb_serial_jtag_vfs_use_driver()` for `printf`/`getchar` to work.

- Boot marker: `FORCE4:READY\n` (machine-parseable, data.sh waits for this)
- Response framing: `---BEGIN---\n` ... `---END---\n` around every command response
- Commands: `ls`, `cat <file>`, `rm <file>`, `status`, `transfer`, `ping`, `help`

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
