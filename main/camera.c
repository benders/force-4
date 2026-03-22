#include "sdkconfig.h"
#ifdef CONFIG_FORCE4_CAMERA

#include "camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "camera";

// OV3660 GPIO assignments on XIAO ESP32-S3 Sense board
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

// Use LEDC_TIMER_1 / LEDC_CHANNEL_1 to avoid conflict with the LED driver
// which occupies LEDC_TIMER_0 / LEDC_CHANNEL_0.
#define CAM_LEDC_TIMER   LEDC_TIMER_1
#define CAM_LEDC_CHANNEL LEDC_CHANNEL_1

// Build a camera_config_t with the shared GPIO/clock settings.
static camera_config_t make_config(framesize_t frame_size, int fb_count,
                                   camera_grab_mode_t grab_mode)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = CAM_LEDC_TIMER,
        .ledc_channel = CAM_LEDC_CHANNEL,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = frame_size,
        .jpeg_quality = 12,
        .fb_count     = fb_count,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = grab_mode,
    };
    return config;
}

static void apply_sensor_settings(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
    }
}

bool camera_init(void)
{
    camera_config_t config = make_config(FRAMESIZE_QXGA, 1,
                                         CAMERA_GRAB_WHEN_EMPTY);

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return false;
    }

    apply_sensor_settings();

    // Discard initial frames so AEC/AWB can settle
    for (int i = 0; i < 5; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }

    ESP_LOGI(TAG, "Camera initialized (OV3660, QXGA 2048x1536 JPEG)");
    return true;
}

bool camera_capture_to_sd(const char *path)
{
    // Discard stale buffered frame
    camera_fb_t *stale = esp_camera_fb_get();
    if (stale) {
        esp_camera_fb_return(stale);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        return false;
    }

    ESP_LOGD(TAG, "Frame captured: %zu bytes", fb->len);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        esp_camera_fb_return(fb);
        return false;
    }

    size_t written = fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    esp_camera_fb_return(fb);

    if (written != fb->len) {
        ESP_LOGE(TAG, "Short write: %zu of %zu bytes", written, fb->len);
        return false;
    }

    ESP_LOGD(TAG, "Saved %s (%zu bytes)", path, written);
    return true;
}

bool camera_configure_video(void)
{
#ifdef CONFIG_FORCE4_VIDEO_QXGA
    framesize_t fs = FRAMESIZE_QXGA;
    const char *label = "QXGA 2048x1536";
#else
    framesize_t fs = FRAMESIZE_SVGA;
    const char *label = "SVGA 800x600";
#endif

    camera_config_t config = make_config(fs, 2, CAMERA_GRAB_LATEST);
    esp_err_t err = esp_camera_reconfigure(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reconfigure to video failed: %s", esp_err_to_name(err));
        return false;
    }
    apply_sensor_settings();
    ESP_LOGI(TAG, "Camera reconfigured for video (%s)", label);
    return true;
}

bool camera_configure_photo(void)
{
    camera_config_t config = make_config(FRAMESIZE_QXGA, 1,
                                         CAMERA_GRAB_WHEN_EMPTY);
    esp_err_t err = esp_camera_reconfigure(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reconfigure to photo failed: %s", esp_err_to_name(err));
        return false;
    }
    apply_sensor_settings();
    ESP_LOGI(TAG, "Camera reconfigured for photo (QXGA)");
    return true;
}

#endif /* CONFIG_FORCE4_CAMERA */
