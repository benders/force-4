# Force-4 Agent Instructions

## Build

```bash
./build.sh        # Docker-based (espressif/idf:v5.4), no local ESP-IDF
./flash.sh        # requires host esptool.py
```

Delete `build/` and `sdkconfig` when changing `sdkconfig.defaults` — stale cache will ignore your changes.

## Critical: USB Serial

The XIAO ESP32-S3 wires USB to the **USB Serial/JTAG controller**, NOT USB-OTG.

- Config must be `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (NOT `USB_CDC`)
- `app_main()` must call `usb_serial_jtag_driver_install()` + `usb_serial_jtag_vfs_use_driver()` before any stdio
- Headers: `driver/usb_serial_jtag.h` and `driver/usb_serial_jtag_vfs.h`
- Using `USB_CDC` makes stdout appear to work but **stdin silently fails**

## GPIO mapping

- SDA=GPIO5 (D4), SCL=GPIO6 (D5), Boot=GPIO9 (D10), LED=GPIO21

## Serial protocol

- `serial_cmd_task` prints `FORCE4:READY\n` when ready for input
- Every command response is wrapped in `---BEGIN---\n` / `---END---\n`
- `data.sh` sends `transfer` command before every operation to pause flight logging and activate double-blink LED
- Opening the serial port does NOT trigger a DTR reset on XIAO ESP32-S3 over USB Serial/JTAG — the board continues running

## CSV format (must match force-3)

```
timestamp_ns,ax_g,ay_g,az_g
```

Internal timestamps are microseconds; multiply by 1000 for the CSV nanosecond column.

## Code conventions

- Static buffers for large arrays (not on task stack) — see `s_samples`, `s_log_ring` in `flight_logger.c`
- Task stack sizes are 4096 bytes — be conservative with stack allocations
- ESP_LOGI/ESP_LOGE go to the same USB serial as printf — the `---BEGIN---`/`---END---` framing separates command output from log noise
- `flight_state_t` is `volatile` — set atomically from any task, read from `flight_task`

## Flash I/O and data gaps

**Critical:** SPIFFS sector erases (~200–400 ms) stall both CPU cores by default. At 400 Hz the ADXL375 hardware FIFO (32 samples = 80 ms) overflows during every erase, causing ~160-sample gaps repeating every ~1 s throughout the recording.

Two-part fix — both are required:

1. **`CONFIG_SPI_FLASH_AUTO_SUSPEND=y`** in `sdkconfig.defaults` — the MSPI controller suspends the erase when the CPU needs a cache fill, so Core 1 is not frozen for the full erase duration.

2. **Dual-task architecture** (`flight_task` pinned to Core 1, `log_write_task` pinned to Core 0) with a 4000-sample RAM ring buffer (`s_log_ring`). `flight_task` only reads the FIFO into the ring; `log_write_task` handles all SPIFFS writes. Even if `log_write_task` blocks during an erase, `flight_task` keeps draining the FIFO.

Do not write to SPIFFS from `flight_task`. Any synchronous flash call (including `fflush`) from the FIFO-reading task will reintroduce gaps.

## Flight file lifecycle

- A zero-length "ready" file (`flight_NNN.csv`) is pre-opened at boot and after each cooldown — no file-open latency at launch detection.
- The next flight number is persisted in NVS (namespace `force4`, key `flight_num`) so it survives SPIFFS wipes.
- Numbers wrap 999 → 000.
- On boot, if the NVS counter points to a non-empty file (crash mid-flight), the counter is advanced to preserve the partial recording.
- `flight_logger_enter_transfer()` closes the ready file before `serial_cmd` can delete files.

## Style

- Markdown tables must have columns padded with spaces so they align in monospace (while remaining legal Markdown)
