# ADXL375 ±200g Accelerometer

3-axis MEMS accelerometer with ±200g full range. Available on Adafruit breakout with STEMMA QT connector. Datasheet: `ADXL375.pdf` in this directory.

## Interface

- **I2C address:** 0x53 (ALT: 0x1D when SDO/CS pulled high)
- **SPI:** also supported (4-wire)
- **Max I2C clock:** 400 kHz

## Scale and output

- **Scale factor:** 49 mg/LSB (fixed, unlike ADXL345)
- **Conversion:** `g = raw * 0.049`
- **Resolution:** 13-bit (right-justified in 16-bit signed register pairs)

## Key registers

| Register          | Address | Purpose                                        |
|-------------------|---------|------------------------------------------------|
| DEVID             | 0x00    | Device ID — always 0xE5 (use to verify comms)  |
| BW_RATE           | 0x2C    | Output data rate and low-power mode            |
| POWER_CTL         | 0x2D    | Measurement/standby, sleep, auto-sleep         |
| INT_ENABLE        | 0x2E    | Enable interrupts on INT1/INT2                 |
| INT_MAP           | 0x2F    | Route interrupts to INT1 (0) or INT2 (1) pins  |
| INT_SOURCE        | 0x30    | Read interrupt source; **clears and re-arms**  |
| DATA_FORMAT       | 0x31    | Range, justification, full resolution          |
| DATAX0/1..DATAZ0/1| 0x32–37 | Raw axis readings (6 bytes, burst-readable)    |
| FIFO_CTL          | 0x38    | FIFO mode and watermark                        |
| FIFO_STATUS       | 0x39    | Number of samples in FIFO                      |
| ACT_INACT_CTL     | 0x27    | Activity/inactivity coupling (AC vs DC)        |
| THRESH_ACT        | 0x24    | Activity threshold (780 mg/LSB)                |
| TIME_INACT        | 0x26    | Inactivity time (1 s/LSB)                      |
| THRESH_INACT      | 0x25    | Inactivity threshold (780 mg/LSB)              |

## Output data rates (BW_RATE 0x2C, low bits)

| Code | ODR     |
|------|---------|
| 0x0A | 100 Hz  |
| 0x0B | 200 Hz  |
| 0x0C | 400 Hz  |
| 0x0D | 800 Hz  |

## FIFO

- Hardware FIFO: **32 samples** deep
- Modes (FIFO_CTL bits 7:6): Bypass (00), FIFO (01), Stream (10), Trigger (11)
- **Stream mode** continuously overwrites oldest samples — no overflow flag, but data is lost if not drained before full
- Watermark level set in FIFO_CTL bits 4:0; FIFO_STATUS reports current fill level
- Burst-read all 6 axis bytes per sample in one transaction to minimize bus time

## Activity detection

The ADXL375 can assert INT1/INT2 when it detects motion above a threshold.

### DC vs AC coupling (ACT_INACT_CTL)

- **DC-coupled (bit 7 = 0):** threshold compared against absolute acceleration. Gravity (~1g, up to ~3g on a noisy axis in any orientation) can permanently trigger this.
- **AC-coupled (bit 7 = 1):** threshold compared against the *change* from a baseline. Gravity is cancelled; only a change from the resting state triggers the interrupt. Useful for orientation-agnostic wake detection.

The baseline is captured **when `INT_SOURCE` is read** — this re-arms the detector from whatever the current acceleration is. Always read `INT_SOURCE` from a resting state before relying on activity detection.

### THRESH_ACT

Scale: **780 mg/LSB**. Use `ceilf()` when converting from g to avoid rounding down below the noise floor:

```c
uint8_t thresh = (uint8_t)ceilf(threshold_g / 0.780f);
```

For post-reset recovery and ESP-IDF driver details, see `reference/I2C.md`.

## Initialization sequence

```
1. Power on or reset recovery (see reference/I2C.md)
2. Verify DEVID == 0xE5
3. Write POWER_CTL = 0x00   (standby)
4. Write DATA_FORMAT = 0x0B (full resolution, right-justified, ±200g)
5. Write BW_RATE = 0x0C     (400 Hz)
6. Configure FIFO_CTL, INT_ENABLE, INT_MAP as needed
7. Write POWER_CTL = 0x08   (measurement mode)
```
