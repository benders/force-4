#include "storage.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "storage";
static const char *MOUNT_POINT = "/spiffs";

bool storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %zu total, %zu used, %zu free",
             total, used, total - used);
    return true;
}

int storage_next_flight_number(void)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) return 0;

    int max_n = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int n;
        char extra;
        if (sscanf(ent->d_name, "flight_%d%c", &n, &extra) == 1) {
            if (n > max_n) max_n = n;
        }
    }
    closedir(dir);
    return max_n + 1;
}

int storage_load_flight_counter(void)
{
    nvs_handle_t h;
    if (nvs_open("force4", NVS_READONLY, &h) == ESP_OK) {
        int32_t n = 0;
        esp_err_t err = nvs_get_i32(h, "flight_num", &n);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Flight counter from NVS: %d", (int)n);
            return (int)n;
        }
    }
    // NVS key not yet set — derive from dir scan and persist
    int n = storage_next_flight_number();
    storage_save_flight_counter(n);
    ESP_LOGI(TAG, "Flight counter from dir scan: %d (saved to NVS)", n);
    return n;
}

void storage_save_flight_counter(int n)
{
    nvs_handle_t h;
    if (nvs_open("force4", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "flight_num", (int32_t)n);
        nvs_commit(h);
        nvs_close(h);
    }
}

FILE *storage_open_flight(int n)
{
    char path[48];
    snprintf(path, sizeof(path), "%s/flight_%03d", MOUNT_POINT, n);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return NULL;
    }

    ESP_LOGI(TAG, "Opened %s for writing", path);
    return f;
}

void storage_write_samples(FILE *f, const adxl375_sample_t *samples, int count)
{
    if (!f) return;
    for (int i = 0; i < count; i++) {
        flight_record_t rec = {
            .timestamp_us = samples[i].timestamp_us,
            .ax_g = samples[i].ax_g,
            .ay_g = samples[i].ay_g,
            .az_g = samples[i].az_g,
        };
        fwrite(&rec, sizeof(rec), 1, f);
    }
}

void storage_close_flight(FILE *f)
{
    if (!f) return;
    fflush(f);
    fclose(f);
    ESP_LOGI(TAG, "Flight file closed");
}

size_t storage_free_space(void)
{
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    return total - used;
}

void storage_list_flights(void)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        printf("No flights.\n");
        return;
    }

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        char path[280];
        snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            printf("%s  %ld\n", ent->d_name, (long)st.st_size);
            count++;
        }
    }
    closedir(dir);
    if (count == 0) {
        printf("No flights.\n");
    }
}

bool storage_format(void)
{
    esp_err_t err = esp_spiffs_format("storage");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS format failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SPIFFS formatted and remounted");
    return true;
}

bool storage_delete_file(const char *filename)
{
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, filename);
    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Deleted %s", path);
        return true;
    }
    ESP_LOGE(TAG, "Failed to delete %s: %s", path, strerror(errno));
    return false;
}
