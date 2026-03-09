#include "flight_logger.h"
#include "adxl375.h"
#include "storage.h"
#include "led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "flight";

// Configuration (matching force-3 config.py)
#define SAMPLE_RATE_HZ      400
#define LAUNCH_THRESHOLD_G  3.0f
#define LAUNCH_HOLD_SAMPLES 20      // 50ms at 400Hz
#define PRE_BUF_SIZE        800     // 2s * 400Hz
#define RECORD_DURATION_US  (60 * 1000000LL)
#define IDLE_COOLDOWN_MS    10000
#define WRITE_BUF_FLUSH     256

static volatile flight_state_t state = FLIGHT_STATE_IDLE;
static volatile int flight_count = 0;

// Ring buffer for pre-trigger data
static adxl375_sample_t pre_buf[PRE_BUF_SIZE];

// Static buffers to keep large arrays off the task stack
static adxl375_sample_t s_samples[32];
static adxl375_sample_t s_write_buf[WRITE_BUF_FLUSH];
static int pre_buf_head = 0;
static int pre_buf_count = 0;

static void pre_buf_push(const adxl375_sample_t *s)
{
    pre_buf[pre_buf_head] = *s;
    pre_buf_head = (pre_buf_head + 1) % PRE_BUF_SIZE;
    if (pre_buf_count < PRE_BUF_SIZE) pre_buf_count++;
}

static void pre_buf_drain_to_file(FILE *f)
{
    if (!f || pre_buf_count == 0) return;

    // Oldest sample is at (head - count) mod size
    int start = (pre_buf_head - pre_buf_count + PRE_BUF_SIZE) % PRE_BUF_SIZE;
    for (int i = 0; i < pre_buf_count; i++) {
        int idx = (start + i) % PRE_BUF_SIZE;
        storage_write_samples(f, &pre_buf[idx], 1);
    }
    fflush(f);
    pre_buf_count = 0;
    pre_buf_head = 0;
}

flight_state_t flight_logger_get_state(void)
{
    return state;
}

int flight_logger_get_flight_count(void)
{
    return flight_count;
}

void flight_logger_enter_transfer(void)
{
    state = FLIGHT_STATE_TRANSFER;
}

void flight_task(void *pvParameters)
{
    (void)pvParameters;

    // Subscribe to task watchdog
    esp_task_wdt_add(NULL);

    adxl375_sample_t *samples = s_samples;
    adxl375_sample_t *write_buf = s_write_buf;
    int write_buf_count = 0;

    int launch_count = 0;
    int64_t logging_start_us = 0;
    FILE *flight_file = NULL;

    ESP_LOGI(TAG, "Flight task started");

    while (1) {
        esp_task_wdt_reset();

        float t_s = (float)(esp_timer_get_time() / 1000) / 1000.0f;

        int n = adxl375_read_fifo_batch(samples, 32);
        if (n <= 0) {
            // Update LED even when no samples
            if (state == FLIGHT_STATE_IDLE) {
                led_breathe_update(t_s);
            } else if (state == FLIGHT_STATE_LOGGING) {
                led_flash_update(t_s);
            } else if (state == FLIGHT_STATE_TRANSFER) {
                led_transfer_update(t_s);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        switch (state) {
        case FLIGHT_STATE_IDLE:
            led_breathe_update(t_s);

            for (int i = 0; i < n; i++) {
                pre_buf_push(&samples[i]);

                float mag = sqrtf(samples[i].ax_g * samples[i].ax_g +
                                  samples[i].ay_g * samples[i].ay_g +
                                  samples[i].az_g * samples[i].az_g);

                if (mag > LAUNCH_THRESHOLD_G) {
                    launch_count++;
                    if (launch_count >= LAUNCH_HOLD_SAMPLES) {
                        int flight_num = storage_next_flight_number();
                        flight_file = storage_open_flight(flight_num);
                        if (flight_file) {
                            pre_buf_drain_to_file(flight_file);
                            state = FLIGHT_STATE_LOGGING;
                            logging_start_us = esp_timer_get_time();
                            write_buf_count = 0;
                            flight_count++;
                            ESP_LOGI(TAG, "Launch detected! Recording flight_%03d", flight_num);
                        } else {
                            ESP_LOGE(TAG, "Skipping flight — could not open file");
                            launch_count = 0;
                        }
                        break;
                    }
                } else {
                    launch_count = 0;
                }
            }
            break;

        case FLIGHT_STATE_LOGGING:
            led_flash_update(t_s);

            for (int i = 0; i < n; i++) {
                write_buf[write_buf_count++] = samples[i];
                if (write_buf_count >= WRITE_BUF_FLUSH) {
                    storage_write_samples(flight_file, write_buf, write_buf_count);
                    fflush(flight_file);
                    write_buf_count = 0;
                }
            }

            int64_t elapsed = esp_timer_get_time() - logging_start_us;
            if (elapsed >= RECORD_DURATION_US) {
                // Flush remaining samples
                if (write_buf_count > 0) {
                    storage_write_samples(flight_file, write_buf, write_buf_count);
                    write_buf_count = 0;
                }
                storage_close_flight(flight_file);
                flight_file = NULL;

                ESP_LOGI(TAG, "Recording complete. Cooling down...");
                state = FLIGHT_STATE_COOLDOWN;

                led_blink_n(3);
                led_off();
                vTaskDelay(pdMS_TO_TICKS(IDLE_COOLDOWN_MS));

                state = FLIGHT_STATE_IDLE;
                launch_count = 0;
                ESP_LOGI(TAG, "Ready. Waiting for launch...");
            }
            break;

        case FLIGHT_STATE_COOLDOWN:
            // Handled inline above after transition
            break;

        case FLIGHT_STATE_TRANSFER:
            led_transfer_update(t_s);
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }
    }
}
