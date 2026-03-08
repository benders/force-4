#pragma once

#include <stdio.h>
#include "adxl375.h"

/**
 * Mount SPIFFS partition. Format if mount fails.
 * Returns true on success.
 */
bool storage_init(void);

/**
 * Scan existing flight files and return the next number.
 */
int storage_next_flight_number(void);

/**
 * Open a new flight CSV file. Writes CSV header.
 * Returns FILE* or NULL on failure.
 */
FILE *storage_open_flight(int n);

/**
 * Write samples as CSV lines to an open flight file.
 */
void storage_write_samples(FILE *f, const adxl375_sample_t *samples, int count);

/**
 * Flush and close a flight file.
 */
void storage_close_flight(FILE *f);

/**
 * Return bytes free on SPIFFS.
 */
size_t storage_free_space(void);

/**
 * Log flight file listing to serial.
 */
void storage_list_flights(void);

/**
 * Delete a file from SPIFFS. Returns true on success.
 */
bool storage_delete_file(const char *filename);
