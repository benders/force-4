#pragma once
#include <stdbool.h>

// FreeRTOS task: blocks until video_start() is called, then captures frames
// to an AVI file on the SD card until video_stop() signals it to finish.
void video_task(void *pvParameters);

// Signal the video task to begin recording.  The task handles camera
// reconfiguration and file creation on Core 0 (non-blocking from the
// caller's perspective after the notification is sent).
void video_start(int flight_num);

// Signal the video task to stop recording (non-blocking).  The task
// finalizes the AVI file and reconfigures the camera back to photo mode.
void video_stop(void);

// Block until video recording is fully finished (AVI finalized, camera
// restored to photo mode).  Safe to call even if not recording.
void video_wait(void);

// Returns true if video is currently recording.
bool video_is_recording(void);
