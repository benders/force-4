#include "adxl375.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "adxl375";

// Registers
#define REG_DEVID           0x00
#define REG_BW_RATE         0x2C
#define REG_POWER_CTL       0x2D
#define REG_DATA_FORMAT     0x31
#define REG_THRESH_ACT      0x24
#define REG_ACT_INACT_CTL   0x27
#define REG_INT_ENABLE      0x2E
#define REG_INT_MAP         0x2F
#define REG_INT_SOURCE      0x30
#define REG_DATAX0          0x32
#define REG_FIFO_CTL        0x38
#define REG_FIFO_STATUS     0x39

// Config values
#define DEVID_EXPECTED      0xE5
#define BW_RATE_800HZ       0x0D
#define POWER_CTL_MEASURE   0x08
#define DATA_FORMAT_FULL    0x0B
#define FIFO_STREAM_WM16    0x90  // Stream mode (0b10 << 6) | watermark 16
#define ACT_CTL_AC_XYZ      0xF0  // AC-coupled activity, X+Y+Z enabled
#define ACT_THRESH_SCALE    0.78f // g per LSB for THRESH_ACT

#define MG_PER_LSB          0.049f

// I2C addresses (selected by SDO pin: low=0x53, high=0x1D)
#define ADXL375_ADDR_PRIMARY  0x53
#define ADXL375_ADDR_ALT      0x1D

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

#define I2C_TIMEOUT_MS 100

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C bus scan:");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
        }
    }
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
    if (write_reg(REG_BW_RATE, BW_RATE_800HZ) != ESP_OK) return false;
    if (write_reg(REG_POWER_CTL, POWER_CTL_MEASURE) != ESP_OK) return false;
    if (write_reg(REG_DATA_FORMAT, DATA_FORMAT_FULL) != ESP_OK) return false;
    if (write_reg(REG_FIFO_CTL, FIFO_STREAM_WM16) != ESP_OK) return false;

    // Interrupts: all mapped to INT1 (default), disabled until flight_task
    if (write_reg(REG_INT_MAP, 0x00) != ESP_OK) return false;
    if (write_reg(REG_INT_ENABLE, 0x00) != ESP_OK) return false;

    // Activity detection: AC-coupled so gravity offset is ignored regardless
    // of orientation. Threshold set later by config_activity_int.
    if (write_reg(REG_ACT_INACT_CTL, ACT_CTL_AC_XYZ) != ESP_OK) return false;

    ESP_LOGI(TAG, "Sensor configured: 800Hz, stream FIFO, watermark=16");
    return true;
}

static bool try_address(uint16_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device 0x%02X failed: %s", addr, esp_err_to_name(err));
        return false;
    }

    if (configure_sensor()) {
        ESP_LOGI(TAG, "ADXL375 found at I2C address 0x%02X", addr);
        return true;
    }

    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

bool adxl375_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;

    if (try_address(ADXL375_ADDR_PRIMARY)) return true;
    if (try_address(ADXL375_ADDR_ALT)) return true;

    ESP_LOGE(TAG, "ADXL375 not found at either I2C address");
    i2c_scan();
    return false;
}

bool adxl375_reinit(void)
{
    if (!s_bus) return false;

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    // Reset the I2C bus to recover from stuck-SDA conditions
    i2c_master_bus_reset(s_bus);
    vTaskDelay(pdMS_TO_TICKS(50));

    return adxl375_init(s_bus);
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
        buf[read_count].timestamp_us = t_now - (int64_t)(count - 1 - i) * 1250;
        read_count++;
    }

    return read_count;
}

void adxl375_config_activity_int(float threshold_g)
{
    uint8_t thresh = (uint8_t)ceilf(threshold_g / ACT_THRESH_SCALE);
    if (thresh == 0) thresh = 1;
    write_reg(REG_THRESH_ACT, thresh);

    // Clear any pending interrupt flags
    uint8_t dummy;
    read_reg(REG_INT_SOURCE, &dummy, 1);

    // Enable activity interrupt only (bit 4)
    write_reg(REG_INT_ENABLE, 0x10);
    ESP_LOGI(TAG, "Activity interrupt enabled: %.1fg (reg=%d)", thresh * ACT_THRESH_SCALE, thresh);
}

void adxl375_config_watermark_int(void)
{
    // Clear any pending interrupt flags
    uint8_t dummy;
    read_reg(REG_INT_SOURCE, &dummy, 1);

    // Enable watermark interrupt only (bit 1)
    write_reg(REG_INT_ENABLE, 0x02);
}

uint8_t adxl375_read_int_source(void)
{
    uint8_t src = 0;
    read_reg(REG_INT_SOURCE, &src, 1);
    return src;
}
