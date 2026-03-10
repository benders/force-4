#include "adxl375.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "adxl375";

#define ADXL375_ADDR        0x53

// Registers
#define REG_DEVID           0x00
#define REG_BW_RATE         0x2C
#define REG_POWER_CTL       0x2D
#define REG_DATA_FORMAT     0x31
#define REG_DATAX0          0x32
#define REG_FIFO_CTL        0x38
#define REG_FIFO_STATUS     0x39

// Config values
#define DEVID_EXPECTED      0xE5
#define BW_RATE_400HZ       0x0C
#define POWER_CTL_MEASURE   0x08
#define DATA_FORMAT_FULL    0x0B
#define FIFO_STREAM_WM16    0x90  // Stream mode (0b10 << 6) | watermark 16

#define MG_PER_LSB          0.049f

#define I2C_TIMEOUT_MS      100
#define I2C_RETRY_COUNT     3

static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;
static gpio_num_t s_sda = GPIO_NUM_NC;
static gpio_num_t s_scl = GPIO_NUM_NC;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_RETRY_COUNT; i++) {
        err = i2c_master_transmit(dev_handle, buf, 2, I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "I2C write retry %d for reg 0x%02X: %s", i + 1, reg, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_RETRY_COUNT; i++) {
        err = i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "I2C read retry %d for reg 0x%02X: %s", i + 1, reg, esp_err_to_name(err));
    }
    return err;
}

static bool configure_sensor(void)
{
    // Verify device ID
    uint8_t devid = 0;
    if (read_reg(REG_DEVID, &devid, 1) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read DEVID");
        return false;
    }
    if (devid != DEVID_EXPECTED) {
        ESP_LOGE(TAG, "Bad DEVID: 0x%02X (expected 0x%02X)", devid, DEVID_EXPECTED);
        return false;
    }
    ESP_LOGI(TAG, "DEVID=0x%02X OK", devid);

    // Configure sensor
    if (write_reg(REG_BW_RATE, BW_RATE_400HZ) != ESP_OK) return false;
    if (write_reg(REG_POWER_CTL, POWER_CTL_MEASURE) != ESP_OK) return false;
    if (write_reg(REG_DATA_FORMAT, DATA_FORMAT_FULL) != ESP_OK) return false;
    if (write_reg(REG_FIFO_CTL, FIFO_STREAM_WM16) != ESP_OK) return false;

    ESP_LOGI(TAG, "Sensor configured: 400Hz, stream FIFO, watermark=16");
    return true;
}

/* Scan the bus to find any responding device and log what we find. */
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        esp_err_t err = i2c_master_probe(bus_handle, addr, 20);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  No I2C devices found — check wiring/power");
    }
}

bool adxl375_init(gpio_num_t sda, gpio_num_t scl)
{
    s_sda = sda;
    s_scl = scl;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Reset bus to free any device holding SDA low from a prior session.
    // 50ms lets the ADXL375 finish any in-progress transaction after an ESP32
    // soft reset (button press), when the sensor does not lose power.
    i2c_master_bus_reset(bus_handle);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Probe for the ADXL375: try primary addr 0x53, fallback to 0x1D
    uint8_t found_addr = 0;
    uint8_t candidates[] = {0x53, 0x1D};
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(bus_handle, candidates[i], 20) == ESP_OK) {
            found_addr = candidates[i];
            ESP_LOGI(TAG, "ADXL375 found at 0x%02X", found_addr);
            break;
        }
    }
    if (found_addr == 0) {
        ESP_LOGE(TAG, "ADXL375 not found at 0x53 or 0x1D");
        i2c_scan();
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = found_addr,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return false;
    }

    return configure_sensor();
}

bool adxl375_reinit(void)
{
    if (s_sda == GPIO_NUM_NC) return false;  // adxl375_init never called

    // Tear down existing handles so adxl375_init can rebuild from scratch
    if (dev_handle != NULL) {
        i2c_master_bus_rm_device(dev_handle);
        dev_handle = NULL;
    }
    if (bus_handle != NULL) {
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
    }
    return adxl375_init(s_sda, s_scl);
}

int adxl375_read_fifo_count(void)
{
    uint8_t status = 0;
    if (read_reg(REG_FIFO_STATUS, &status, 1) != ESP_OK) {
        return -1;
    }
    return status & 0x3F;
}

int adxl375_read_fifo_batch(adxl375_sample_t *buf, int max_samples)
{
    int count = adxl375_read_fifo_count();
    if (count <= 0) return count;
    if (count > max_samples) count = max_samples;

    int64_t t_now = esp_timer_get_time();
    int read_count = 0;

    for (int i = 0; i < count; i++) {
        uint8_t raw[6];
        if (read_reg(REG_DATAX0, raw, 6) != ESP_OK) {
            break;
        }
        int16_t x = (int16_t)(raw[0] | (raw[1] << 8));
        int16_t y = (int16_t)(raw[2] | (raw[3] << 8));
        int16_t z = (int16_t)(raw[4] | (raw[5] << 8));

        buf[read_count].ax_g = x * MG_PER_LSB;
        buf[read_count].ay_g = y * MG_PER_LSB;
        buf[read_count].az_g = z * MG_PER_LSB;
        // Timestamps: oldest sample first
        buf[read_count].timestamp_us = t_now - (int64_t)(count - 1 - i) * 2500;
        read_count++;
    }

    return read_count;
}
