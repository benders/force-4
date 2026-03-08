#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "led.h"
#include "adxl375.h"
#include "storage.h"
#include "flight_logger.h"
#include "serial_cmd.h"

static const char *TAG = "main";

// XIAO ESP32-S3: D10=GPIO9, D4=GPIO5 (SDA), D5=GPIO6 (SCL)
#define GPIO_BOOT_MODE  GPIO_NUM_9
#define GPIO_SDA        GPIO_NUM_5
#define GPIO_SCL        GPIO_NUM_6

#define FLIGHT_TASK_STACK  4096
#define SERIAL_TASK_STACK  4096

static bool g_flight_mode = true;

void app_main(void)
{
    ESP_LOGI(TAG, "Force-4 booting");

    // Read boot mode from GPIO10 (internal pull-up, LOW = data mode)
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_BOOT_MODE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(10)); // Let pin settle
    g_flight_mode = gpio_get_level(GPIO_BOOT_MODE) == 1;
    ESP_LOGI(TAG, "Boot mode: %s", g_flight_mode ? "FLIGHT" : "DATA");

    // Initialize LED
    led_init();

    // Initialize storage
    if (!storage_init()) {
        ESP_LOGE(TAG, "Storage init failed!");
    }

    // Initialize sensor
    bool sensor_ok = adxl375_init(GPIO_SDA, GPIO_SCL);
    if (!sensor_ok) {
        ESP_LOGE(TAG, "ADXL375 init failed — will retry");
        // Retry loop for sensor reconnection
        for (int i = 0; i < 60 && !sensor_ok; i++) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            sensor_ok = adxl375_reinit();
            if (sensor_ok) {
                ESP_LOGI(TAG, "ADXL375 connected after retry");
            }
        }
    }

    if (g_flight_mode) {
        ESP_LOGI(TAG, "Starting flight mode");

        if (sensor_ok) {
            xTaskCreatePinnedToCore(
                flight_task, "flight", FLIGHT_TASK_STACK,
                NULL, 5, NULL, 1);
        } else {
            ESP_LOGE(TAG, "No sensor — flight task not started");
        }

        xTaskCreate(
            serial_cmd_task, "serial_cmd", SERIAL_TASK_STACK,
            &g_flight_mode, 3, NULL);
    } else {
        ESP_LOGI(TAG, "Starting data mode");

        // Brief LED flash to indicate data mode
        led_set(8191);
        vTaskDelay(pdMS_TO_TICKS(1000));
        led_off();

        xTaskCreate(
            serial_cmd_task, "serial_cmd", SERIAL_TASK_STACK,
            &g_flight_mode, 3, NULL);
    }

    ESP_LOGI(TAG, "Force-4 ready");
}
