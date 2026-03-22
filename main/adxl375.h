#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef struct {
    int64_t timestamp_us;
    float ax_g, ay_g, az_g;
} adxl375_sample_t;

/**
 * Initialize ADXL375 sensor on an already-initialized I2C bus.
 * Probes both addresses (0x53, 0x1D) and configures the sensor.
 * Returns true on success, false if sensor not found.
 */
bool adxl375_init(i2c_master_bus_handle_t bus);

/**
 * Reset the I2C bus, remove and re-add the device, then reconfigure.
 */
bool adxl375_reinit(void);

/**
 * Read number of samples available in FIFO.
 * Returns -1 on error.
 */
int adxl375_read_fifo_count(void);

/**
 * Read up to max_samples from FIFO into buf.
 * Returns number of samples read, or -1 on error.
 */
int adxl375_read_fifo_batch(adxl375_sample_t *buf, int max_samples);

/**
 * Configure ADXL375 to generate an activity interrupt on INT1.
 * Threshold in g (resolution 780 mg/LSB, minimum ~0.78g).
 * Disables all other interrupts.
 */
void adxl375_config_activity_int(float threshold_g);

/**
 * Configure ADXL375 to generate a watermark interrupt on INT1.
 * Uses the watermark level set during init (16 samples).
 * Disables all other interrupts.
 */
void adxl375_config_watermark_int(void);

/**
 * Read INT_SOURCE register to determine which interrupt fired
 * and clear latched interrupt flags.
 */
uint8_t adxl375_read_int_source(void);
