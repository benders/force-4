# Force-4

Native C (ESP-IDF) rocket flight data logger. Records +-200g accelerometer data at 400 Hz to onboard flash. Successor to [force-3](../force-3) (CircuitPython).

## Hardware

- **Board:** Seeed XIAO ESP32-S3 (8 MB flash)
- **Sensor:** Adafruit ADXL375 +-200g accelerometer (I2C, 0x53)
- **LED:** Built-in on GPIO21

### Wiring

| XIAO Pin     | Connection                                |
|--------------|-------------------------------------------|
| D4 (GPIO5)   | ADXL375 SDA (STEMMA QT)                   |
| D5 (GPIO6)   | ADXL375 SCL (STEMMA QT)                   |
| D10 (GPIO9)  | Boot mode select (float=flight, GND=data) |

The ADXL375 STEMMA QT cable provides SDA, SCL, 3V3, and GND. No other wiring needed.

## Build

Requires Docker. No local ESP-IDF install.

```bash
./build.sh
```

## Flash

Requires `esptool.py` on the host (`pip install esptool`).

```bash
./flash.sh          # flash + monitor
./flash.sh monitor  # monitor only
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
./data.sh state                # show device state (non-invasive)
./data.sh diag                 # verify device health
./data.sh df                   # show storage usage and flight list
./data.sh pull                 # download most recent flight
./data.sh pull flight_001.csv  # download specific file
./data.sh wipe                 # delete all flights
./data.sh wipe flight_001.csv  # delete specific file
```

Requires `pyserial` (`pip install pyserial`).

No DTR reset occurs on XIAO ESP32-S3 over USB Serial/JTAG — the board keeps running when the port is opened. `data.sh` sends a `transfer` command to pause flight logging before any operation, and a `resume` command when done to return the device to IDLE.

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
