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
adxl375.c/.h      ADXL375 sensor driver (SPI, 4 MHz, Mode 3; see reference/ADXL375.md)
flight_logger.c/.h  State machine + ring buffer + CSV recording
storage.c/.h      SPIFFS mount, flight file lifecycle, binary record writing
serial_cmd.c/.h   Command dispatch, response framing
led.c/.h          LEDC PWM patterns (breathe, flash, transfer, blink)
```

## State machine

```
IDLE --(|a| > 3g for 50ms)--> LOGGING --(60s)--> COOLDOWN --(3s)--> IDLE
  |  \--("trigger" cmd)----->/                                        |
  |                  "transfer" cmd (from any IDLE state)             |
  +----------- "resume" cmd or 30s timeout <-- TRANSFER <------------+
```

## Flight recording pipeline

1. `flight_task` (Core 1) drains ADXL375 FIFO (up to 32 samples/read via SPI burst)
2. In IDLE: samples go into a 1600-entry pre-trigger ring buffer (2s at 800 Hz)
3. On launch detect: pre-trigger buffer + live samples are pushed into a 16,000-entry PSRAM ring buffer (`s_log_ring`)
4. `log_write_task` (Core 0) drains `s_log_ring` → binary records on SPIFFS. Flash erase stalls only Core 0; `flight_task` keeps reading the FIFO uninterrupted
5. After 60s: `flight_task` signals flush, `log_write_task` closes the file, 3s cooldown, return to IDLE

## Serial protocol

Bidirectional over USB Serial/JTAG controller (not USB-OTG). Requires `usb_serial_jtag_driver_install()` + `usb_serial_jtag_vfs_use_driver()` for `printf`/`getchar` to work.

- Boot marker: `FORCE4:READY\n` (printed at startup for diagnostics; mission-control does not wait for it)
- Response framing: `---BEGIN---\n` ... `---END---\n` around every command response
- Commands: `ls`, `cat <file>`, `rm <file>`, `status`, `trigger`, `transfer`, `resume`, `ping`, `help`
- `cat` returns a `size:<N>\n` header line followed by raw binary (512-byte chunks via `fwrite`). LF→CRLF conversion is disabled before binary output and restored after — without this, VFS inserts `\r` before every `\n` (0x0A) byte in the binary data. `mission-control pull` uses `Device.read_binary()` to read exactly N bytes with progress output, then converts binary → CSV locally

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
| BW_RATE (0x2C)      | 0x0D  | 800 Hz ODR                       |
| POWER_CTL (0x2D)    | 0x08  | Measurement mode                 |
| DATA_FORMAT (0x31)  | 0x0B  | Full resolution, right-justified |
| FIFO_CTL (0x38)     | 0x90  | Stream mode, watermark=16        |

Scale: 49 mg/LSB (raw * 0.049 = g). Timestamps estimated backward from read time at 1250us intervals.

General ADXL375 notes (activity detection, SPI framing, register reference): `reference/ADXL375.md`.

### Connection recovery

SPI has no stuck-bus problem (CS idles high, no shared clock line). If `adxl375_init()` fails (bad DEVID or SPI error), `main.c` retries every 5s for up to 5 minutes via `adxl375_reinit()`, which removes the SPI device, frees the bus, and calls `adxl375_init()` from scratch.

## Interrupt-driven idle

ADXL375 INT1 → GPIO4 drives `flight_task` via FreeRTOS task notification instead of polling.

| State         | INT1 source | Behavior                                              |
|---------------|-------------|-------------------------------------------------------|
| IDLE sleeping | Activity    | Task blocked; wakes on accel change > 2.34g every 2s |
| IDLE active   | Watermark   | Polls FIFO, fills pre-buffer, detects launch          |
| LOGGING       | Watermark   | Blocks until 16 samples ready (20ms at 800 Hz)        |
| TRANSFER      | (any)       | FIFO drained, interrupts ignored                      |

After 5 seconds of quiet (< 2g) in active IDLE, `flight_task` reconfigures for activity interrupt and blocks again.

Activity detection uses **AC-coupled mode** (`ACT_INACT_CTL` bit 7 = 1): the chip measures change from a baseline, not absolute acceleration. This ignores gravity in any orientation. The baseline is captured when `INT_SOURCE` is read, which also re-arms the detector. Both `adxl375_config_activity_int()` and the 2s polling fallback read `INT_SOURCE`, so the baseline is always taken from a resting state.

**Edge-trigger race:** INT1 is POSEDGE-triggered. If the pin goes high between arming and `ulTaskNotifyTake`, the edge is missed. The 2s timeout path reads `INT_SOURCE` as a fallback — if the activity bit is set, active polling is entered regardless of whether the ISR fired.

`flight_logger_enter_transfer()` calls `xTaskNotifyGive()` to wake `flight_task` if it is blocked waiting for an interrupt.

The ISR handler (`int1_isr_handler`) is IRAM-resident and only calls `vTaskNotifyGiveFromISR`.

## Flash I/O and data gaps

SPIFFS sector erases (200–400 ms) stall both CPU cores by default. At 800 Hz the ADXL375 FIFO (32 samples = 40 ms) overflows during every erase, causing ~320-sample gaps repeating every ~1 s. Three fixes are all required:

1. **`CONFIG_SPI_FLASH_AUTO_SUSPEND=y`** — the MSPI controller suspends the erase when the CPU needs a cache fill, so Core 1 is not frozen for the full erase duration. See `reference/XIAO-ESP32S3.md`.

2. **Dual-task architecture** — `flight_task` (Core 1) only reads the FIFO into `s_log_ring`; `log_write_task` (Core 0) handles all SPIFFS writes. Even if `log_write_task` blocks during an erase, `flight_task` keeps draining the FIFO uninterrupted.

3. **PSRAM ring buffer** — binary records are 20 bytes/sample, so 800 Hz requires ~16 KB/s, well within SPIFFS's ~21 KB/s sustained throughput. `s_log_ring` holds 16,000 entries (~20 s at 800 Hz) allocated from PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, absorbing erase stalls (200–400 ms each) rather than compensating for a throughput deficit.

Do not write to SPIFFS from `flight_task`. Any synchronous flash call (including `fflush`) from the FIFO-reading task reintroduces gaps.

### PSRAM and AUTO_SUSPEND compatibility

`CONFIG_SPIRAM=y` and `CONFIG_SPI_FLASH_AUTO_SUSPEND=y` are compatible on ESP32-S3 and must both be enabled. **Do not use `EXT_RAM_BSS_ATTR`** to place large arrays in PSRAM — the BSS zero-fill that runs during startup (before `app_main()` and before the USB Serial/JTAG driver is installed) conflicts with early boot and causes a silent crash with no serial output. Use `heap_caps_malloc(MALLOC_CAP_SPIRAM)` inside `flight_task` instead.

## Flight file lifecycle

- A zero-length "ready" file (`flight_NNN`) is pre-opened at boot and after each cooldown — no file-open latency at launch detection.
- Flight files contain packed binary records (`flight_record_t`, 20 bytes each: `int64_t timestamp_us`, `float ax_g`, `float ay_g`, `float az_g`). Python struct format: `'<qfff'`.
- `mission-control pull` fetches raw binary via `cat` and converts to CSV locally.
- Flight number is persisted in NVS (namespace `force4`, key `flight_num`) and survives SPIFFS wipes.
- Numbers wrap 999 → 000.
- On boot, if the NVS counter points to a non-empty file (crash mid-flight), the counter is advanced to preserve the partial recording.
- `flight_logger_enter_transfer()` closes the ready file before `serial_cmd` can delete files.

## Code conventions

- Large arrays use static buffers (not task stack) — see `s_samples` in `flight_logger.c`. Exception: `s_log_ring` is heap-allocated from PSRAM at task startup; see PSRAM note above.
- Task stack size is 4096 bytes — be conservative with stack allocations; avoid large local arrays.
- `ESP_LOGI`/`ESP_LOGE` share the USB serial with `printf`. The `---BEGIN---`/`---END---` framing in command responses separates command output from log noise.
- `flight_state_t` is `volatile` — set atomically from any task, read only from `flight_task`.

General XIAO ESP32-S3 notes (USB Serial/JTAG setup, flash erase stalls): `reference/XIAO-ESP32S3.md`.
