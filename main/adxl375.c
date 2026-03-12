#include "adxl375.h"
#include "driver/spi_master.h"
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

// SPI address byte framing (ADXL375 protocol):
//   bit 7: R/W (1=read, 0=write)
//   bit 6: MB  (1=multi-byte burst)
//   bits 5:0: register address
#define SPI_READ            0x80
#define SPI_MB              0x40

static spi_device_handle_t s_spi = NULL;
static gpio_num_t s_mosi = GPIO_NUM_NC;
static gpio_num_t s_miso = GPIO_NUM_NC;
static gpio_num_t s_sclk = GPIO_NUM_NC;
static gpio_num_t s_cs   = GPIO_NUM_NC;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {reg & 0x3F, val};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    uint8_t tx[8] = {0};
    uint8_t rx[8] = {0};

    tx[0] = SPI_READ | (len > 1 ? SPI_MB : 0) | (reg & 0x3F);

    spi_transaction_t t = {
        .length = (1 + len) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK) {
        memcpy(data, &rx[1], len);
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

bool adxl375_init(gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t cs)
{
    s_mosi = mosi;
    s_miso = miso;
    s_sclk = sclk;
    s_cs   = cs;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev_cfg = {
        .mode = 3,               // CPOL=1, CPHA=1
        .clock_speed_hz = 4000000,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return false;
    }

    return configure_sensor();
}

bool adxl375_reinit(void)
{
    if (s_mosi == GPIO_NUM_NC) return false;  // adxl375_init never called

    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    spi_bus_free(SPI2_HOST);

    return adxl375_init(s_mosi, s_miso, s_sclk, s_cs);
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
