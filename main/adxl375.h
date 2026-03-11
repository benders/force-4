#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

typedef struct {
    int64_t timestamp_us;
    float ax_g, ay_g, az_g;
} adxl375_sample_t;

/**
 * Initialize SPI bus and ADXL375 sensor.
 * Returns true on success, false if sensor not found.
 */
bool adxl375_init(gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t cs);

/**
 * Tear down and re-initialize SPI bus + sensor (used for reconnect after failure).
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
