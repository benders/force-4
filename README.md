# Force-4

Native C (ESP-IDF) rocket flight data logger. Records +-200g accelerometer data at 800 Hz to onboard flash, with optional MJPEG video recording to SD card. Successor to [force-3](../force-3) (CircuitPython).

## Hardware

- **Board:** Seeed XIAO ESP32-S3 (8 MB flash)
- **Sensor:** Adafruit ADXL375 +-200g accelerometer (I2C, 400 kHz)
- **LED:** External on D0 (GPIO1), active-high, 330Ω resistor to GND

### Wiring

| XIAO Pin    | Connection                                |
|-------------|-------------------------------------------|
| D4 (GPIO5)  | ADXL375 SDA (on breakout board)          |
| D5 (GPIO6)  | ADXL375 SCL (on breakout board)          |
| D3 (GPIO4)  | ADXL375 INT1                             |
| D0 (GPIO1)  | LED anode via 330Ω resistor (to GND)     |
| 3V3         | ADXL375 VIN + CS (tie CS high for I2C)   |
| GND         | ADXL375 GND + SDO (tie SDO low for 0x53) |

## Build

Requires Docker. No local ESP-IDF install.

```bash
./build.sh
```

## Flash

Requires `esptool.py` on the host (`pip install esptool`).

```bash
./flash.sh                    # flash firmware
./mission-control monitor     # monitor serial output
```

## Operation

### Boot modes

- **D10 floating (default):** Flight mode -- LED pulses, logger armed
- **D10 grounded:** Data mode -- no logging

### LED patterns

| Pattern                 | State                     |
|-------------------------|---------------------------|
| Brief pulse every 2s    | Idle, waiting for launch  |
| Fast flash (5 Hz)       | Recording flight          |
| 3 blinks then off       | Cooldown after recording  |
| Double-blink (2s cycle) | Data transfer in progress |

### Flight recording

The logger continuously monitors acceleration magnitude. When it exceeds 3g for 50ms, it records 60 seconds of data (including 2 seconds of pre-trigger buffer) to a CSV file on SPIFFS.

## Data transfer

```bash
./mission-control status               # show device status
./mission-control monitor              # stream raw serial output (Ctrl-C to stop)
./mission-control diag                 # verify device health
./mission-control df                   # show storage usage and flight list
./mission-control pull                 # download all new flights (skips already-pulled)
./mission-control pull flight_001      # download specific file
./mission-control wipe                 # delete all flights
./mission-control wipe flight_001.csv  # delete specific file
```

No third-party packages required — uses only Python stdlib.

No DTR reset occurs on XIAO ESP32-S3 over USB Serial/JTAG — the board keeps running when the port is opened. `monitor` is read-only and can be used any time. Data commands (`pull`, `wipe`, `df`, `ls`, `cat`, `rm`, `diag`) enter the TRANSFER state for the duration and resume IDLE when done. All commands except `status` and `monitor` require the device to be IDLE before starting.

## Plot

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
jupyter notebook plot_flight.ipynb
```

### CSV format

```
timestamp_ns,ax_g,ay_g,az_g
151163940441,1.0290,-0.1470,0.1470
```
