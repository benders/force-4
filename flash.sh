#!/usr/bin/env bash
# Flash and monitor the ESP32-S3.
# Docker USB passthrough on macOS is limited, so we use host-side tools.
set -e

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "ERROR: No USB modem device found. Is the board connected?"
  exit 1
fi

case "${1:-flash}" in
  flash)
    echo "Flashing via $PORT ..."
    esptool.py --chip esp32s3 --port "$PORT" \
      --baud 460800 --before default_reset --after hard_reset \
      write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
      0x0 build/bootloader/bootloader.bin \
      0x8000 build/partition_table/partition-table.bin \
      0x10000 build/force-4.bin \
      0x190000 build/storage.bin 2>/dev/null || \
    esptool.py --chip esp32s3 --port "$PORT" \
      --baud 460800 --before default_reset --after hard_reset \
      write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
      0x0 build/bootloader/bootloader.bin \
      0x8000 build/partition_table/partition-table.bin \
      0x10000 build/force-4.bin
    ;;
  monitor)
    ;;
  *)
    echo "Usage: $0 [flash|monitor]"
    exit 1
    ;;
esac

echo "Starting monitor on $PORT ..."
screen "$PORT" 115200
