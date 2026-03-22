#include "sdkconfig.h"
#ifdef CONFIG_FORCE4_CAMERA

#include "video.h"
#include "sdcard.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "video";

// Maximum frames in the index (supports up to 2 minutes at 10 fps)
#define MAX_INDEX_ENTRIES 1200

// AVI header size is fixed at 224 bytes (no audio stream)
#define AVI_HEADER_SIZE 224

// ---------------------------------------------------------------------------
// AVI index entry
// ---------------------------------------------------------------------------
typedef struct {
    char     ckid[4];       // "00dc"
    uint32_t dwFlags;       // 0x10 = AVIIF_KEYFRAME
    uint32_t dwOffset;      // offset from start of 'movi' list data
    uint32_t dwChunkLength; // size of JPEG data (excluding chunk header)
} avi_idx_entry_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static volatile bool s_recording = false;
static volatile bool s_stop_requested = false;
static TaskHandle_t  s_video_task_handle = NULL;

// AVI recording state (only accessed from video_task)
static FILE           *s_avi_file = NULL;
static avi_idx_entry_t *s_index = NULL;
static uint32_t        s_frame_count = 0;
static uint32_t        s_max_frame_size = 0;
static uint32_t        s_movi_offset = 0;  // file offset where 'movi' data starts

#ifdef CONFIG_FORCE4_VIDEO_QXGA
static const uint32_t VIDEO_WIDTH  = 2048;
static const uint32_t VIDEO_HEIGHT = 1536;
#else
static const uint32_t VIDEO_WIDTH  = 800;
static const uint32_t VIDEO_HEIGHT = 600;
#endif

// ---------------------------------------------------------------------------
// AVI RIFF/header helpers — all values little-endian
// ---------------------------------------------------------------------------

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_tag(uint8_t *p, const char *tag)
{
    memcpy(p, tag, 4);
}

// Write the 224-byte AVI header.  On the first call (before recording)
// frame_count and max_frame_size are 0; they are fixed up after recording.
static bool avi_write_header(FILE *f, uint32_t width, uint32_t height,
                             uint32_t fps_scale, uint32_t frame_count,
                             uint32_t max_frame_size, uint32_t file_size)
{
    uint8_t hdr[AVI_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));

    uint32_t us_per_frame = fps_scale;  // microseconds per frame
    uint32_t movi_list_size = file_size - AVI_HEADER_SIZE - 8;
    // The -8 accounts for the idx1 chunk which is outside the movi LIST,
    // but we don't know its size yet during initial write.  The fixup pass
    // recalculates this properly.

    // RIFF header
    int o = 0;
    put_tag(&hdr[o], "RIFF"); o += 4;
    put_u32(&hdr[o], file_size > 8 ? file_size - 8 : 0); o += 4;
    put_tag(&hdr[o], "AVI "); o += 4;

    // LIST hdrl
    put_tag(&hdr[o], "LIST"); o += 4;
    put_u32(&hdr[o], 192); o += 4;  // hdrl list payload size
    put_tag(&hdr[o], "hdrl"); o += 4;

    // avih (main AVI header) — 56 bytes
    put_tag(&hdr[o], "avih"); o += 4;
    put_u32(&hdr[o], 56); o += 4;           // chunk size
    put_u32(&hdr[o], us_per_frame); o += 4;  // dwMicroSecPerFrame
    put_u32(&hdr[o], 0); o += 4;            // dwMaxBytesPerSec (0 = unspecified)
    put_u32(&hdr[o], 0); o += 4;            // dwPaddingGranularity
    put_u32(&hdr[o], 0x10); o += 4;         // dwFlags: AVIF_HASINDEX
    put_u32(&hdr[o], frame_count); o += 4;  // dwTotalFrames
    put_u32(&hdr[o], 0); o += 4;            // dwInitialFrames
    put_u32(&hdr[o], 1); o += 4;            // dwStreams
    put_u32(&hdr[o], max_frame_size); o += 4; // dwSuggestedBufferSize
    put_u32(&hdr[o], width); o += 4;        // dwWidth
    put_u32(&hdr[o], height); o += 4;       // dwHeight
    o += 16; // dwReserved[4]

    // LIST strl (stream list)
    put_tag(&hdr[o], "LIST"); o += 4;
    put_u32(&hdr[o], 116); o += 4;  // strl list payload size
    put_tag(&hdr[o], "strl"); o += 4;

    // strh (stream header) — 56 bytes
    put_tag(&hdr[o], "strh"); o += 4;
    put_u32(&hdr[o], 56); o += 4;
    put_tag(&hdr[o], "vids"); o += 4;       // fccType
    put_tag(&hdr[o], "MJPG"); o += 4;       // fccHandler
    put_u32(&hdr[o], 0); o += 4;            // dwFlags
    put_u32(&hdr[o], 0); o += 4;            // wPriority + wLanguage
    put_u32(&hdr[o], 0); o += 4;            // dwInitialFrames
    put_u32(&hdr[o], us_per_frame); o += 4;  // dwScale
    put_u32(&hdr[o], 1000000); o += 4;      // dwRate  (fps = dwRate/dwScale)
    put_u32(&hdr[o], 0); o += 4;            // dwStart
    put_u32(&hdr[o], frame_count); o += 4;  // dwLength
    put_u32(&hdr[o], max_frame_size); o += 4; // dwSuggestedBufferSize
    put_u32(&hdr[o], 0xFFFFFFFF); o += 4;   // dwQuality (-1 = default)
    put_u32(&hdr[o], 0); o += 4;            // dwSampleSize
    put_u32(&hdr[o], 0); o += 4;            // rcFrame (left, top)
    put_u32(&hdr[o], (height << 16) | width); o += 4; // rcFrame (right, bottom)

    // strf (stream format) — BITMAPINFOHEADER 40 bytes
    put_tag(&hdr[o], "strf"); o += 4;
    put_u32(&hdr[o], 40); o += 4;
    put_u32(&hdr[o], 40); o += 4;           // biSize
    put_u32(&hdr[o], width); o += 4;        // biWidth
    put_u32(&hdr[o], height); o += 4;       // biHeight
    put_u32(&hdr[o], (24 << 16) | 1); o += 4; // biPlanes(1) + biBitCount(24)
    put_tag(&hdr[o], "MJPG"); o += 4;       // biCompression
    put_u32(&hdr[o], width * height * 3); o += 4; // biSizeImage
    put_u32(&hdr[o], 0); o += 4;            // biXPelsPerMeter
    put_u32(&hdr[o], 0); o += 4;            // biYPelsPerMeter
    put_u32(&hdr[o], 0); o += 4;            // biClrUsed
    put_u32(&hdr[o], 0); o += 4;            // biClrImportant

    // LIST movi header (chunk data follows immediately)
    put_tag(&hdr[o], "LIST"); o += 4;
    put_u32(&hdr[o], movi_list_size); o += 4;
    put_tag(&hdr[o], "movi"); o += 4;

    if (o != AVI_HEADER_SIZE) {
        ESP_LOGE(TAG, "AVI header size mismatch: %d != %d", o, AVI_HEADER_SIZE);
        return false;
    }

    fseek(f, 0, SEEK_SET);
    return fwrite(hdr, 1, AVI_HEADER_SIZE, f) == AVI_HEADER_SIZE;
}

// Write one JPEG frame as an AVI '00dc' chunk.  Returns bytes written
// (including chunk header and padding), or 0 on error.
static size_t avi_write_frame(FILE *f, const uint8_t *jpeg, size_t len)
{
    uint8_t chunk_hdr[8];
    put_tag(chunk_hdr, "00dc");
    put_u32(chunk_hdr + 4, (uint32_t)len);

    if (fwrite(chunk_hdr, 1, 8, f) != 8) return 0;
    if (fwrite(jpeg, 1, len, f) != len) return 0;

    // RIFF chunks are word-aligned; pad with a zero byte if size is odd
    size_t total = 8 + len;
    if (len & 1) {
        uint8_t pad = 0;
        if (fwrite(&pad, 1, 1, f) != 1) return 0;
        total++;
    }
    return total;
}

// Write the idx1 chunk from the in-memory index array.
static bool avi_write_index(FILE *f, const avi_idx_entry_t *idx,
                            uint32_t count)
{
    uint8_t chunk_hdr[8];
    put_tag(chunk_hdr, "idx1");
    put_u32(chunk_hdr + 4, count * 16);

    if (fwrite(chunk_hdr, 1, 8, f) != 8) return false;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[16];
        memcpy(entry, idx[i].ckid, 4);
        put_u32(entry + 4, idx[i].dwFlags);
        put_u32(entry + 8, idx[i].dwOffset);
        put_u32(entry + 12, idx[i].dwChunkLength);
        if (fwrite(entry, 1, 16, f) != 16) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool video_is_recording(void)
{
    return s_recording;
}

bool video_start(int flight_num)
{
    if (s_recording) return false;
    if (!sdcard_is_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted");
        return false;
    }

    // Allocate index buffer from PSRAM
    if (!s_index) {
        s_index = heap_caps_malloc(MAX_INDEX_ENTRIES * sizeof(avi_idx_entry_t),
                                   MALLOC_CAP_SPIRAM);
        if (!s_index) {
            ESP_LOGE(TAG, "Failed to allocate index buffer");
            return false;
        }
    }

    char path[32];
    snprintf(path, sizeof(path), "/sd/flight_%03d.avi", flight_num);

    s_avi_file = fopen(path, "wb");
    if (!s_avi_file) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    // Estimate ~10 fps for SVGA, ~2 fps for QXGA as initial header value.
    // The fixup pass will correct this based on actual timing.
#ifdef CONFIG_FORCE4_VIDEO_QXGA
    uint32_t us_per_frame = 500000;  // 2 fps
#else
    uint32_t us_per_frame = 100000;  // 10 fps
#endif

    s_frame_count = 0;
    s_max_frame_size = 0;
    s_movi_offset = 0;

    if (!avi_write_header(s_avi_file, VIDEO_WIDTH, VIDEO_HEIGHT,
                          us_per_frame, 0, 0, 0)) {
        ESP_LOGE(TAG, "Failed to write AVI header");
        fclose(s_avi_file);
        s_avi_file = NULL;
        return false;
    }

    s_stop_requested = false;
    s_recording = true;

    // Wake the video task
    if (s_video_task_handle) {
        xTaskNotifyGive(s_video_task_handle);
    }

    ESP_LOGI(TAG, "Video recording started: %s (%ux%u)",
             path, (unsigned)VIDEO_WIDTH, (unsigned)VIDEO_HEIGHT);
    return true;
}

void video_stop(void)
{
    if (!s_recording) return;
    s_stop_requested = true;

    // Wait for the task to finish writing
    while (s_recording) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// video_task — capture loop
// ---------------------------------------------------------------------------
void video_task(void *pvParameters)
{
    (void)pvParameters;
    s_video_task_handle = xTaskGetCurrentTaskHandle();

    for (;;) {
        // Sleep until video_start() wakes us
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_recording || !s_avi_file) continue;

        int64_t start_us = esp_timer_get_time();
        uint32_t movi_data_size = 0;  // bytes written inside the movi LIST

        // Discard first frame (may be stale from before reconfigure)
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);

        while (!s_stop_requested) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                ESP_LOGW(TAG, "Frame capture failed, retrying");
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            uint32_t jpeg_len = (uint32_t)fb->len;

            if (s_frame_count < MAX_INDEX_ENTRIES) {
                // Record index entry before writing (offset is relative to
                // the start of the movi list data, after the 'movi' tag)
                s_index[s_frame_count].ckid[0] = '0';
                s_index[s_frame_count].ckid[1] = '0';
                s_index[s_frame_count].ckid[2] = 'd';
                s_index[s_frame_count].ckid[3] = 'c';
                s_index[s_frame_count].dwFlags = 0x10; // AVIIF_KEYFRAME
                s_index[s_frame_count].dwOffset = movi_data_size + 4;
                s_index[s_frame_count].dwChunkLength = jpeg_len;
            }

            size_t written = avi_write_frame(s_avi_file, fb->buf, fb->len);
            esp_camera_fb_return(fb);

            if (written == 0) {
                ESP_LOGE(TAG, "Frame write failed at frame %u", (unsigned)s_frame_count);
                break;
            }

            movi_data_size += written;

            if (jpeg_len > s_max_frame_size) {
                s_max_frame_size = jpeg_len;
            }
            s_frame_count++;
        }

        // Finalize: write index, fix up header
        int64_t elapsed_us = esp_timer_get_time() - start_us;

        if (s_avi_file && s_frame_count > 0) {
            // Write idx1 chunk
            uint32_t idx_count = s_frame_count < MAX_INDEX_ENTRIES
                                 ? s_frame_count : MAX_INDEX_ENTRIES;
            avi_write_index(s_avi_file, s_index, idx_count);

            // Calculate actual microseconds per frame
            uint32_t actual_us_per_frame = (uint32_t)(elapsed_us / s_frame_count);

            // Get total file size
            long file_size = ftell(s_avi_file);

            // Fix up the header with actual values
            // The movi LIST size = 4 (for 'movi' tag) + movi_data_size
            // We need to recalculate the RIFF size properly
            avi_write_header(s_avi_file, VIDEO_WIDTH, VIDEO_HEIGHT,
                             actual_us_per_frame, s_frame_count,
                             s_max_frame_size, (uint32_t)file_size);

            // Fix up the movi LIST size field (at offset AVI_HEADER_SIZE - 8)
            uint8_t movi_size_buf[4];
            put_u32(movi_size_buf, movi_data_size + 4); // +4 for 'movi' tag
            fseek(s_avi_file, AVI_HEADER_SIZE - 8, SEEK_SET);
            fwrite(movi_size_buf, 1, 4, s_avi_file);

            fclose(s_avi_file);
            s_avi_file = NULL;

            uint32_t actual_fps_x10 = (s_frame_count * 10000000ULL) / (uint32_t)(elapsed_us);
            ESP_LOGI(TAG, "Video saved: %u frames, %u.%u fps, %ld bytes",
                     (unsigned)s_frame_count,
                     (unsigned)(actual_fps_x10 / 10),
                     (unsigned)(actual_fps_x10 % 10),
                     file_size);
        } else if (s_avi_file) {
            fclose(s_avi_file);
            s_avi_file = NULL;
            ESP_LOGW(TAG, "Video aborted: no frames captured");
        }

        s_recording = false;
    }
}

#endif /* CONFIG_FORCE4_CAMERA */
