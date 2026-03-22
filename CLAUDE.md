# Force-4 Agent Instructions

## Build

```bash
./build.sh                    # Docker-based (espressif/idf:v5.4), no local ESP-IDF
./flash.sh                    # requires host esptool.py
./mission-control monitor     # serial output after flashing
```

Delete `build/` and `sdkconfig` when changing `sdkconfig.defaults` — stale cache will ignore your changes.

## Critical: USB Serial

The XIAO ESP32-S3 uses the **USB Serial/JTAG controller**, not USB-OTG. See `reference/XIAO-ESP32S3.md` for full details.

- Config: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (NOT `USB_CDC`)
- `app_main()` must call `usb_serial_jtag_driver_install()` + `usb_serial_jtag_vfs_use_driver()` before any stdio
- Using `USB_CDC` makes stdout appear to work but **stdin silently fails**

## GPIO mapping

SCLK=GPIO7 (D8), MOSI=GPIO9 (D10), MISO=GPIO8 (D9), CS=GPIO2 (D1), INT1=GPIO4 (D3), Boot=GPIO9 (D10, read before SPI init), LED=GPIO1 (D0, active-high, external), SD_CS=GPIO21 (Sense board)

Camera (Sense board, OV2640/OV3660): XCLK=GPIO10, SIOD=GPIO40, SIOC=GPIO39, PCLK=GPIO13, VSYNC=GPIO38, HREF=GPIO47, D0–D7=GPIO15/17/18/16/14/12/11/48. LED uses LEDC_TIMER_0/CHANNEL_0; camera XCLK uses LEDC_TIMER_1/CHANNEL_1.

## Serial protocol

- Every command response is wrapped in `---BEGIN---\n` / `---END---\n`
- `serial_cmd_task` prints `FORCE4:READY\n` at boot
- **Binary output must disable VFS newline conversion** (`usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF)`) before `fwrite` and restore CRLF after — otherwise the VFS inserts `\r` before every `0x0A` byte in binary data
- **Binary transfers use a two-step receiver-initiated protocol**: `cat <file>` returns `ready size:N` (no binary); the host then sends `go` to trigger the raw byte stream. `abort` cancels a pending transfer. `go` is the only command with no `---BEGIN---`/`---END---` framing.
- **`tcflush(TCIFLUSH)` does NOT clear the USB hardware FIFO** — it only clears the kernel RX buffer. Bytes already latched in the USB FIFO arrive at the host milliseconds later, after the flush returns. Any protocol that streams binary immediately after a text response is inherently racy; the two-step protocol avoids this by never starting a binary stream until the host explicitly requests it.
- See `ARCHITECTURE.md` for full command list and framing details

## CSV format (must match force-3)

```
timestamp_ns,ax_g,ay_g,az_g
```

Flight data is stored as packed binary records on the device (`flight_NNN`, no `.csv` extension). `mission-control pull` converts binary → CSV on the host. Internal timestamps are microseconds; multiply by 1000 for the CSV nanosecond column.

## PSRAM

The ring buffer (`s_log_ring`, 16,000 entries) is allocated from PSRAM with `heap_caps_malloc(MALLOC_CAP_SPIRAM)` inside `flight_task`. **Do not use `EXT_RAM_BSS_ATTR`** — BSS zero-fill during startup crashes the device silently before USB Serial/JTAG is available. When changing PSRAM-related `sdkconfig.defaults` keys, always `rm -rf build sdkconfig` before rebuilding.

## Flashing

The XIAO ESP32-S3 USB Serial/JTAG controller does **not** support hard-reset via the RTS pin. After holding BOOT+RESET to enter download mode, the device stays in download mode until the **RESET button is pressed again** (not a software reset). `--after hard_reset` in esptool is a no-op here.

Do not open the serial port from two processes simultaneously — concurrent access corrupts the USB Serial/JTAG output stream and causes garbled serial responses.

After power-on, allow ~12 s before running `mission-control` commands. On first boot after a full flash erase, SPIFFS formats, which adds several more seconds.

## mission-control

After `wipe` or `rm`, wait ~3 s before the next `status` call — trailing log bytes in the serial buffer can confuse response parsing. The `wipe` command retries `resume` up to 3 times internally; if all retries fail the device auto-resumes after its 30 s TRANSFER timeout.

## SD card (optional)

Enabled by `CONFIG_FORCE4_SD_CARD` in `main/Kconfig.projbuild`. SD card shares SPI2_HOST with the ADXL375 (CS=GPIO21, Mode 0). SPI bus is initialized in `main.c`; `adxl375_reinit()` must not free the shared bus. When changing this Kconfig option, `rm -rf build sdkconfig` before rebuilding.

- **`#ifdef CONFIG_*` guards at the top of `.c` files** must be preceded by `#include "sdkconfig.h"` — ESP-IDF does not auto-include it; the guard evaluates false and the entire file compiles empty (silently) if the include is missing.
- **FAT uppercases filenames** — host-side searches must be case-insensitive.
- **Space calculations require `uint64_t`** — `size_t` is 32-bit on ESP32-S3 and overflows on cards larger than ~4 GiB.
- **`sys/statvfs.h` is unavailable** in ESP-IDF's newlib — use the FATFS `f_getfree()` API instead.

## Architecture and code conventions

See `ARCHITECTURE.md` for: state machine, tasks, modules, PSRAM ring buffer, interrupt-driven idle, flash I/O gap fix, flight file lifecycle, SD card module, and code conventions.

## Style

- Markdown tables must have columns padded with spaces so they align in monospace (while remaining legal Markdown)

## Documentation

- Documentation should be updated when the code is updated, and must ALWAYS be updated before changes are committed.
- Documentation should be concise and avoid repetition. If information is in another file, reference it, don't duplicate it.
