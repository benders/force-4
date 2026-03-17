#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "driver/spi_master.h"

#include "led.h"
#include "adxl375.h"
#include "storage.h"
#include "flight_logger.h"
#include "serial_cmd.h"
#ifdef CONFIG_FORCE4_SD_CARD
#include "sdcard.h"
#endif

static const char *TAG = "main";

// XIAO ESP32-S3 GPIO assignments
// D10=GPIO9: boot mode (read at startup) + SPI MOSI (after adxl375_init)
// D8=GPIO7: SCLK, D9=GPIO8: MISO, D1=GPIO2: CS, D3=GPIO4: INT1
#define GPIO_BOOT_MODE  GPIO_NUM_9
#define GPIO_SPI_MOSI   GPIO_NUM_9
#define GPIO_SPI_MISO   GPIO_NUM_8
#define GPIO_SPI_SCLK   GPIO_NUM_7
#define GPIO_SPI_CS     GPIO_NUM_2

#define FLIGHT_TASK_STACK  4096
#define SERIAL_TASK_STACK  4096

static bool g_flight_mode = true;

void app_main(void)
{
    ESP_LOGI(TAG, "Force-4 booting");

    // Install USB Serial/JTAG driver so stdin/stdout work bidirectionally
    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 1024,
        .tx_buffer_size = 1024,
    };
    usb_serial_jtag_driver_install(&usb_cfg);
    usb_serial_jtag_vfs_use_driver();

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

    // Initialize NVS (required before storage_init uses NVS for flight counter)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize storage
    if (!storage_init()) {
        ESP_LOGE(TAG, "Storage init failed!");
    }

    // Initialize shared SPI bus (ADXL375 + optional SD card share SPI2_HOST)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_SPI_MOSI,
        .miso_io_num = GPIO_SPI_MISO,
        .sclk_io_num = GPIO_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t spi_err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (spi_err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(spi_err));
    }

    // Initialize sensor (bus already initialized)
    bool sensor_ok = adxl375_init_on_bus(GPIO_SPI_CS);
    if (!sensor_ok) {
        ESP_LOGE(TAG, "ADXL375 init failed — will retry");
        for (int i = 0; i < 60 && !sensor_ok; i++) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            sensor_ok = adxl375_reinit();
            if (sensor_ok) {
                ESP_LOGI(TAG, "ADXL375 connected after retry");
            }
        }
    }

#ifdef CONFIG_FORCE4_SD_CARD
    if (!sdcard_init()) {
        ESP_LOGW(TAG, "SD card not available");
    }
#endif

    if (g_flight_mode) {
        ESP_LOGI(TAG, "Starting flight mode");

        if (sensor_ok) {
            // log_write_task on Core 0: handles all SPIFFS writes so flight_task
            // (Core 1) is never blocked by flash erase operations.
            xTaskCreatePinnedToCore(
                log_write_task, "log_write", FLIGHT_TASK_STACK,
                NULL, 5, NULL, 0);
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
