#pragma once
#include <stdbool.h>

// Initialize the OV3660 camera. Returns true on success.
bool camera_init(void);

// Capture a JPEG frame and save it to path on the SD card.
// Returns true on success.
bool camera_capture_to_sd(const char *path);

// Reconfigure camera for video capture (resolution from Kconfig, 2 frame
// buffers, CAMERA_GRAB_LATEST). Returns true on success.
bool camera_configure_video(void);

// Reconfigure camera for photo capture (QXGA, 1 frame buffer,
// CAMERA_GRAB_WHEN_EMPTY). Returns true on success.
bool camera_configure_photo(void);
