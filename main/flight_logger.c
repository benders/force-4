#include "flight_logger.h"
#include "adxl375.h"
#include "storage.h"
#include "led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"

static const char *TAG = "flight";

// Configuration (matching force-3 config.py)
#define SAMPLE_RATE_HZ      800
#define LAUNCH_THRESHOLD_G  3.0f
#define LAUNCH_HOLD_SAMPLES 40          // 50ms at 800Hz
#define PRE_BUF_SIZE        1600        // 2s * 800Hz
#define RECORD_DURATION_US  (60 * 1000000LL)
#define IDLE_COOLDOWN_MS    3000

// Ring buffer that decouples FIFO reads (flight_task, Core 1) from
// SPIFFS writes (log_write_task, Core 0).  16000 samples ≈ 20s at 800Hz.
// Sized to absorb the SPIFFS throughput deficit (~168 samples/sec) over a
// full 60s flight (deficit = ~10080 samples; ring gives 14400 of headroom).
// Allocated from PSRAM heap at runtime (see flight_task startup).
#define LOG_RING_SIZE       16000

// Interrupt-driven idle: ADXL375 INT1 on GPIO4 (XIAO D3)
#define GPIO_INT1           GPIO_NUM_4
#define ACTIVITY_THRESHOLD_G  2.0f      // wake threshold (below launch)
#define ACTIVITY_QUIET_G    2.0f        // below this = "quiet" (0 resting samples reach 2g)
#define IDLE_QUIET_US       (5LL * 1000000LL)  // 5s quiet → sleep

static volatile flight_state_t state = FLIGHT_STATE_IDLE;
static volatile int flight_count = 0;

// Transfer-mode exit: set by flight_logger_exit_transfer() or on timeout.
#define TRANSFER_TIMEOUT_US  (30LL * 1000000LL)
static volatile bool s_exit_transfer = false;
static int64_t s_transfer_entry_us = 0;

// Manual flight trigger: set by flight_logger_trigger() from serial_cmd.
static volatile bool s_manual_trigger = false;

// Pre-trigger ring buffer
static adxl375_sample_t pre_buf[PRE_BUF_SIZE];
static int pre_buf_head = 0;
static int pre_buf_count = 0;

// FIFO read scratch buffer (small, only used inside flight_task)
static adxl375_sample_t s_samples[32];

// Log ring buffer — written by flight_task, consumed by log_write_task.
// Allocated from PSRAM at startup (heap_caps_malloc MALLOC_CAP_SPIRAM).
static adxl375_sample_t *s_log_ring = NULL;
static volatile uint32_t s_ring_head = 0;   // producer index (flight_task)
static volatile uint32_t s_ring_tail = 0;   // consumer index (log_write_task)

// Set by flight_task when recording ends; cleared by log_write_task after
// it drains the ring and closes the file.
static volatile bool s_log_flush = false;

// setvbuf backing store for the flight file
static char s_file_iobuf[8192];

// Pre-opened flight file and its number — opened during IDLE so there is
// zero file-open latency at launch detection.
static FILE *flight_file = NULL;
static int s_flight_num = 0;

// Interrupt state
static TaskHandle_t s_flight_task_handle = NULL;
static bool s_idle_active = false;  // true = polling FIFO, false = waiting on INT
static int64_t s_last_activity_us = 0;

static void IRAM_ATTR int1_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t higher_prio_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_flight_task_handle, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

static void pre_buf_push(const adxl375_sample_t *s)
{
    pre_buf[pre_buf_head] = *s;
    pre_buf_head = (pre_buf_head + 1) % PRE_BUF_SIZE;
    if (pre_buf_count < PRE_BUF_SIZE) pre_buf_count++;
}

// Enter low-power activity-interrupt mode and clear any pending notification.
static void enter_idle_sleep(void)
{
    adxl375_config_activity_int(ACTIVITY_THRESHOLD_G);
    s_idle_active = false;
    ulTaskNotifyTake(pdTRUE, 0);  // clear pending
}

// Open (or create) the next ready file — empty CSV, no header yet.
// If the NVS counter points to a file that already has data (crash recovery
// after power loss mid-flight), advance the counter first.
static void prepare_idle_file(void)
{
    int n = storage_load_flight_counter();

    char path[48];
    snprintf(path, sizeof(path), "/spiffs/flight_%03d.csv", n);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        n = (n + 1) % 1000;
        storage_save_flight_counter(n);
    }

    s_flight_num = n;
    flight_file = storage_open_flight(n);
    if (flight_file) {
        setvbuf(flight_file, s_file_iobuf, _IOFBF, sizeof(s_file_iobuf));
        ESP_LOGI(TAG, "Ready file: flight_%03d.csv", n);
    } else {
        ESP_LOGE(TAG, "Failed to open ready file flight_%03d.csv", n);
    }
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
    // Close the idle ready file so serial_cmd can safely delete SPIFFS files
    if (state == FLIGHT_STATE_IDLE && flight_file) {
        fclose(flight_file);
        flight_file = NULL;
    }
    s_exit_transfer = false;
    s_transfer_entry_us = esp_timer_get_time();
    state = FLIGHT_STATE_TRANSFER;

    // Suppress log output so ESP_LOGI lines cannot interleave with serial responses
    esp_log_level_set("*", ESP_LOG_NONE);

    // Wake flight_task if blocked on interrupt wait
    if (s_flight_task_handle) {
        xTaskNotifyGive(s_flight_task_handle);
    }
}

void flight_logger_exit_transfer(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    s_exit_transfer = true;
}

void flight_logger_trigger(void)
{
    if (state != FLIGHT_STATE_IDLE) return;
    s_manual_trigger = true;
    if (s_flight_task_handle) {
        xTaskNotifyGive(s_flight_task_handle);
    }
}

// ---------------------------------------------------------------------------
// log_write_task  (pin to Core 0)
//
// Drains s_log_ring → flight_file.  Never called from flight_task so SPIFFS
// blocking here does not stall the FIFO read loop on Core 1.
//
// With CONFIG_SPI_FLASH_AUTO_SUSPEND=y the flash erase runs in background;
// Core 1 (flight_task) is not stalled during the erase and can keep reading
// the hardware FIFO into s_log_ring without interruption.
// ---------------------------------------------------------------------------
void log_write_task(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        uint32_t head = s_ring_head;
        uint32_t tail = s_ring_tail;

        if (head == tail) {
            // Ring empty — check if we need to finalise the recording
            if (s_log_flush && flight_file) {
                storage_close_flight(flight_file);
                flight_file = NULL;
                s_log_flush = false;
            }
            vTaskDelay(1);
            continue;
        }

        if (!flight_file) {
            // No file to write to; discard
            s_ring_tail = tail + 1;
            continue;
        }

        storage_write_samples(flight_file, &s_log_ring[tail % LOG_RING_SIZE], 1);
        s_ring_tail = tail + 1;
    }
}

// ---------------------------------------------------------------------------
// flight_task  (pin to Core 1)
//
// Interrupt-driven: blocks on ADXL375 INT1 notifications instead of polling.
//
// IDLE sleeping:  activity interrupt at 2g wakes the task.
// IDLE active:    polls FIFO to fill pre-buffer + detect launch.
//                 Returns to sleeping after 5s of quiet (< 1.5g).
// LOGGING:        watermark interrupt every 16 samples (20ms at 800Hz).
//
// Never calls storage_write_samples — all flash I/O is in log_write_task.
// ---------------------------------------------------------------------------
void flight_task(void *pvParameters)
{
    (void)pvParameters;

    s_flight_task_handle = xTaskGetCurrentTaskHandle();

    // Allocate log ring buffer from PSRAM at runtime to avoid BSS placement
    // issues during startup (EXT_RAM_BSS_ATTR zeroing happens before USB CDC
    // is up, making crashes invisible).
    s_log_ring = heap_caps_malloc(LOG_RING_SIZE * sizeof(adxl375_sample_t),
                                  MALLOC_CAP_SPIRAM);
    if (!s_log_ring) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%zu bytes); falling back to DRAM",
                 LOG_RING_SIZE * sizeof(adxl375_sample_t));
        s_log_ring = malloc(LOG_RING_SIZE * sizeof(adxl375_sample_t));
        if (!s_log_ring) {
            ESP_LOGE(TAG, "DRAM alloc also failed — aborting flight_task");
            vTaskDelete(NULL);
            return;
        }
    } else {
        ESP_LOGI(TAG, "Log ring allocated in PSRAM (%u samples)",
                 (unsigned)LOG_RING_SIZE);
    }

    // Configure GPIO interrupt for ADXL375 INT1 (active-high, rising edge)
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_INT1,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_INT1, int1_isr_handler, NULL);

    esp_task_wdt_add(NULL);

    int launch_count = 0;
    int drop_count = 0;
    int64_t logging_start_us = 0;

    // Pre-open the ready file before the main loop so launch detection
    // needs no file-open latency.
    prepare_idle_file();

    // Start in low-power activity-interrupt mode
    enter_idle_sleep();

    ESP_LOGI(TAG, "Flight task started (interrupt-driven)");

    while (1) {
        esp_task_wdt_reset();

        switch (state) {
        case FLIGHT_STATE_IDLE:
            if (s_manual_trigger) {
                s_manual_trigger = false;
                if (flight_file) {
                    // Copy pre-trigger buffer into ring (RAM→RAM)
                    int start = (pre_buf_head - pre_buf_count + PRE_BUF_SIZE) % PRE_BUF_SIZE;
                    for (int j = 0; j < pre_buf_count; j++) {
                        int idx = (start + j) % PRE_BUF_SIZE;
                        uint32_t h = s_ring_head;
                        if (h - s_ring_tail < LOG_RING_SIZE) {
                            s_log_ring[h % LOG_RING_SIZE] = pre_buf[idx];
                            s_ring_head = h + 1;
                        }
                    }
                    pre_buf_count = 0;
                    pre_buf_head = 0;

                    adxl375_config_watermark_int();
                    s_idle_active = false;
                    launch_count = 0;
                    state = FLIGHT_STATE_LOGGING;
                    logging_start_us = esp_timer_get_time();
                    flight_count++;
                    ESP_LOGI(TAG, "Manual trigger! Recording flight_%03d", s_flight_num);
                } else {
                    ESP_LOGE(TAG, "Manual trigger skipped — no ready file");
                }
                break;
            }

            if (!s_idle_active) {
                // Low-power wait: block until activity interrupt or 2s timeout
                uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
                if (state != FLIGHT_STATE_IDLE) break;  // changed to TRANSFER
                // Check for activity: either ISR notification or polled fallback.
                // Polling covers the case where INT1 went high before the
                // wait started (edge already passed, no new POSEDGE).
                uint8_t int_src = adxl375_read_int_source();
                if (notified || (int_src & 0x10)) {
                    adxl375_config_watermark_int();
                    s_idle_active = true;
                    s_last_activity_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "Activity detected — polling");
                } else {
                    // No activity — short LED blink
                    led_set(8191);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    led_off();
                }
                break;
            }

            {   // Active IDLE: poll FIFO, fill pre-buffer, detect launch
                float t_s = (float)(esp_timer_get_time() / 1000) / 1000.0f;
                led_idle_update(t_s);

                int n = adxl375_read_fifo_batch(s_samples, 32);
                if (n <= 0) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                    break;
                }

                for (int i = 0; i < n; i++) {
                    pre_buf_push(&s_samples[i]);

                    float mag = sqrtf(s_samples[i].ax_g * s_samples[i].ax_g +
                                      s_samples[i].ay_g * s_samples[i].ay_g +
                                      s_samples[i].az_g * s_samples[i].az_g);

                    if (mag > ACTIVITY_QUIET_G)
                        s_last_activity_us = esp_timer_get_time();

                    if (mag > LAUNCH_THRESHOLD_G) {
                        launch_count++;
                        if (launch_count >= LAUNCH_HOLD_SAMPLES) {
                            if (flight_file) {
                                // Copy pre-trigger buffer into ring (RAM→RAM, instant)
                                int start = (pre_buf_head - pre_buf_count + PRE_BUF_SIZE) % PRE_BUF_SIZE;
                                for (int j = 0; j < pre_buf_count; j++) {
                                    int idx = (start + j) % PRE_BUF_SIZE;
                                    uint32_t h = s_ring_head;
                                    if (h - s_ring_tail < LOG_RING_SIZE) {
                                        s_log_ring[h % LOG_RING_SIZE] = pre_buf[idx];
                                        s_ring_head = h + 1;
                                    }
                                }
                                pre_buf_count = 0;
                                pre_buf_head = 0;

                                // Watermark interrupt already configured
                                state = FLIGHT_STATE_LOGGING;
                                logging_start_us = esp_timer_get_time();
                                flight_count++;
                                ESP_LOGI(TAG, "Launch! Recording flight_%03d", s_flight_num);
                            } else {
                                ESP_LOGE(TAG, "Skipping flight — no ready file");
                                launch_count = 0;
                            }
                            break;
                        }
                    } else {
                        launch_count = 0;
                    }
                }

                // Return to sleep if quiet for IDLE_QUIET_US
                if (s_idle_active &&
                    (esp_timer_get_time() - s_last_activity_us > IDLE_QUIET_US)) {
                    launch_count = 0;
                    enter_idle_sleep();
                    ESP_LOGI(TAG, "Quiet — waiting for activity");
                }
            }
            break;

        case FLIGHT_STATE_LOGGING:
            {
                // Wait for watermark interrupt (50ms timeout for safety + LED)
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

                float t_s = (float)(esp_timer_get_time() / 1000) / 1000.0f;
                led_flash_update(t_s);

                int n = adxl375_read_fifo_batch(s_samples, 32);
                if (n <= 0) break;

                // Push samples into ring buffer only — no SPIFFS here
                for (int i = 0; i < n; i++) {
                    uint32_t h = s_ring_head;
                    if (h - s_ring_tail < LOG_RING_SIZE) {
                        s_log_ring[h % LOG_RING_SIZE] = s_samples[i];
                        s_ring_head = h + 1;
                        if (drop_count > 0) {
                            ESP_LOGW(TAG, "Ring recovered: %d samples dropped", drop_count);
                            drop_count = 0;
                        }
                    } else {
                        drop_count++;
                    }
                }

                int64_t elapsed = esp_timer_get_time() - logging_start_us;
                if (elapsed >= RECORD_DURATION_US) {
                    ESP_LOGI(TAG, "Recording complete. Flushing...");
                    state = FLIGHT_STATE_COOLDOWN;

                    // Signal log_write_task to drain and close the file
                    s_log_flush = true;

                    // Wait for log_write_task to finish (it sets flight_file=NULL)
                    while (s_log_flush) {
                        esp_task_wdt_reset();
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }

                    led_blink_n(3);
                    led_off();
                    vTaskDelay(pdMS_TO_TICKS(IDLE_COOLDOWN_MS));

                    // Advance counter and open the next ready file
                    int next = (s_flight_num + 1) % 1000;
                    storage_save_flight_counter(next);
                    s_ring_head = 0;
                    s_ring_tail = 0;
                    launch_count = 0;

                    state = FLIGHT_STATE_IDLE;
                    prepare_idle_file();
                    enter_idle_sleep();
                    ESP_LOGI(TAG, "Ready. Waiting for activity...");
                }
            }
            break;

        case FLIGHT_STATE_COOLDOWN:
            // Handled inline above
            break;

        case FLIGHT_STATE_TRANSFER:
            {
                float t_s = (float)(esp_timer_get_time() / 1000) / 1000.0f;
                led_transfer_update(t_s);

                // Drain FIFO to prevent overflow (discard samples)
                adxl375_read_fifo_batch(s_samples, 32);

                if (s_exit_transfer ||
                    (esp_timer_get_time() - s_transfer_entry_us) >= TRANSFER_TIMEOUT_US) {
                    s_exit_transfer = false;
                    ESP_LOGI(TAG, "Exiting transfer mode, returning to IDLE");
                    launch_count = 0;

                    prepare_idle_file();
                    enter_idle_sleep();
                    state = FLIGHT_STATE_IDLE;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            break;
        }
    }
}
