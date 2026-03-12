# Force-4 Agent Instructions

## Build

```bash
./build.sh        # Docker-based (espressif/idf:v5.4), no local ESP-IDF
./flash.sh        # requires host esptool.py
```

Delete `build/` and `sdkconfig` when changing `sdkconfig.defaults` — stale cache will ignore your changes.

## Critical: USB Serial

The XIAO ESP32-S3 uses the **USB Serial/JTAG controller**, not USB-OTG. See `reference/XIAO-ESP32S3.md` for full details.

- Config: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (NOT `USB_CDC`)
- `app_main()` must call `usb_serial_jtag_driver_install()` + `usb_serial_jtag_vfs_use_driver()` before any stdio
- Using `USB_CDC` makes stdout appear to work but **stdin silently fails**

## GPIO mapping

SCLK=GPIO7 (D8), MOSI=GPIO9 (D10), MISO=GPIO8 (D9), CS=GPIO2 (D1), INT1=GPIO4 (D3), Boot=GPIO9 (D10, read before SPI init), LED=GPIO1 (D0, active-high, external)

## Serial protocol

- Every command response is wrapped in `---BEGIN---\n` / `---END---\n`
- `serial_cmd_task` prints `FORCE4:READY\n` at boot
- See `ARCHITECTURE.md` for full command list and framing details

## CSV format (must match force-3)

```
timestamp_ns,ax_g,ay_g,az_g
```

Internal timestamps are microseconds; multiply by 1000 for the CSV nanosecond column.

## PSRAM

The ring buffer (`s_log_ring`, 16,000 entries) is allocated from PSRAM with `heap_caps_malloc(MALLOC_CAP_SPIRAM)` inside `flight_task`. **Do not use `EXT_RAM_BSS_ATTR`** — BSS zero-fill during startup crashes the device silently before USB Serial/JTAG is available. When changing PSRAM-related `sdkconfig.defaults` keys, always `rm -rf build sdkconfig` before rebuilding.

## Flashing

The XIAO ESP32-S3 USB Serial/JTAG controller does **not** support hard-reset via the RTS pin. After holding BOOT+RESET to enter download mode, the device stays in download mode until the **RESET button is pressed again** (not a software reset). `--after hard_reset` in esptool is a no-op here.

Do not open the serial port from two processes simultaneously — concurrent access corrupts the USB Serial/JTAG output stream and causes garbled serial responses.

After power-on, allow ~12 s before running `mission-control` commands. On first boot after a full flash erase, SPIFFS formats, which adds several more seconds.

## mission-control

After `wipe` or `rm`, wait ~3 s before the next `status` call — trailing log bytes in the serial buffer can confuse response parsing. The `wipe` command retries `resume` up to 3 times internally; if all retries fail the device auto-resumes after its 30 s TRANSFER timeout.

When pulling large flight files, `ESP_LOGI` messages from the firmware may be interleaved into the CSV content. Filter non-numeric rows when analysing: `[r for r in csv.DictReader(f) if r['timestamp_ns'].strip().isdigit()]`.

## Architecture and code conventions

See `ARCHITECTURE.md` for: state machine, tasks, modules, PSRAM ring buffer, interrupt-driven idle, flash I/O gap fix, flight file lifecycle, and code conventions.

## Style

- Markdown tables must have columns padded with spaces so they align in monospace (while remaining legal Markdown)

## Documentation

- Documentation should be updated when the code is updated, and must ALWAYS be updated before changes are committed.
- Documentation should be concise and avoid repetition. If information is in another file, reference it, don't duplicate it.
