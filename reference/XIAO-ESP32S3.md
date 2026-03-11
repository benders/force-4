# Seeed XIAO ESP32-S3

Compact ESP32-S3 module with 8 MB flash, 8 MB PSRAM, USB-C, and a STEMMA QT / Qwiic I2C connector.

## USB Serial/JTAG vs USB-OTG

The XIAO ESP32-S3 connects its USB-C port to the **USB Serial/JTAG controller**, not the USB-OTG peripheral. Many ESP32 guides assume USB-OTG (USB_CDC); that does not apply here.

### ESP-IDF configuration

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y   # correct
# CONFIG_ESP_CONSOLE_USB_CDC is NOT y  # wrong for this board
```

### Driver initialization

Call both functions before any stdio use:

```c
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

usb_serial_jtag_driver_config_t cfg = {
    .rx_buffer_size = 1024,
    .tx_buffer_size = 1024,
};
usb_serial_jtag_driver_install(&cfg);
usb_serial_jtag_vfs_use_driver();
```

If `USB_CDC` is used instead, `stdout` appears to work but **`stdin` silently fails** — `getchar()` and `scanf()` block forever or return garbage.

### No DTR reset on port open

Opening the serial port over USB Serial/JTAG does **not** assert DTR or reset the chip. The board keeps running when a host opens the port. If your protocol relies on a reset-on-connect, you must trigger it another way (e.g., a command, a button, or a power cycle).

## GPIO / Pin mapping

| Silk label | GPIO   | Notes                                    |
|------------|--------|------------------------------------------|
| D0         | GPIO1  |                                          |
| D1         | GPIO2  |                                          |
| D2         | GPIO3  |                                          |
| D3         | GPIO4  |                                          |
| D4         | GPIO5  | SDA (STEMMA QT)                          |
| D5         | GPIO6  | SCL (STEMMA QT)                          |
| D6         | GPIO43 | UART TX                                  |
| D7         | GPIO44 | UART RX                                  |
| D8         | GPIO7  | SCK                                      |
| D9         | GPIO8  | MISO / Boot strapping (pull-down = DL)   |
| D10        | GPIO9  | MOSI / Boot strapping (pull-up = normal) |
| LED        | GPIO21 | Built-in LED, **active-low**             |

The STEMMA QT connector brings out GPIO5 (SDA), GPIO6 (SCL), 3V3, and GND — no soldering needed for I2C peripherals.

## Flash and PSRAM

- Flash: 8 MB, quad-SPI, used for firmware + SPIFFS storage partition
- PSRAM: 8 MB, octal-SPI

## Partition layout (default IDF)

The default `partitions.csv` may not reserve enough space for large SPIFFS partitions. Use a custom partition table when the storage region needs to be large. See the [ESP-IDF partition table docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/partition-tables.html).

## SPIFFS and flash erase stalls

SPIFFS sector erases (200–400 ms) stall **both CPU cores** by default because the MSPI bus is shared between flash and the instruction cache. On time-critical workloads (e.g., high-rate sensor logging), enable flash auto-suspend so Core 1 is not frozen:

```
CONFIG_SPI_FLASH_AUTO_SUSPEND=y
```

This allows the MSPI controller to pause an in-progress erase when either core needs a cache fill.
