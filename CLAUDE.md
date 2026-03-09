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

- Static buffers for large arrays (not on task stack) — see `s_samples`, `s_write_buf` in `flight_logger.c`
- Task stack sizes are 4096 bytes — be conservative with stack allocations
- ESP_LOGI/ESP_LOGE go to the same USB serial as printf — the `---BEGIN---`/`---END---` framing separates command output from log noise
- `flight_state_t` is `volatile` — set atomically from any task, read from `flight_task`

## Style

- Markdown tables must have columns padded with spaces so they align in monospace (while remaining legal Markdown)
