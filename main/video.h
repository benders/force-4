#pragma once
#include <stdbool.h>

// FreeRTOS task: blocks until video_start() is called, then captures frames
// to an AVI file on the SD card until video_stop() signals it to finish.
void video_task(void *pvParameters);

// Begin recording video to /sd/FLIGHT_NNN.AVI.  Returns true if the AVI
// file was opened successfully and the task was notified to start capturing.
bool video_start(int flight_num);

// Signal the video task to stop recording.  Blocks until the AVI file is
// finalized (index written, header fixed up, file closed).
void video_stop(void);

// Returns true if video is currently recording.
bool video_is_recording(void);
