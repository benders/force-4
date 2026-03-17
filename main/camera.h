#pragma once
#include <stdbool.h>

// Initialize the OV2640 camera. Returns true on success.
bool camera_init(void);

// Capture a JPEG frame and save it to path on the SD card.
// Returns true on success.
bool camera_capture_to_sd(const char *path);
