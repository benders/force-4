#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"

typedef struct {
    int64_t timestamp_us;
    float ax_g, ay_g, az_g;
} adxl375_sample_t;

/**
 * Initialize I2C bus and ADXL375 sensor.
 * Returns true on success, false if sensor not found.
 */
bool adxl375_init(gpio_num_t sda, gpio_num_t scl);

/**
 * Tear down and re-initialize I2C bus + sensor (used for reconnect after failure).
 * adxl375_init() must have been called at least once to record the GPIO pins.
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
