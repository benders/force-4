#include "sdkconfig.h"
#ifdef CONFIG_FORCE4_SD_CARD

#include "sdcard.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "sdcard";

#define SD_MOUNT_POINT "/sd"
#define SD_CS_GPIO     GPIO_NUM_21

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

bool sdcard_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_GPIO;
    slot_cfg.host_id = SPI2_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount SD card filesystem");
        } else {
            ESP_LOGW(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        s_mounted = false;
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at " SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    return true;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

void sdcard_list_files(void)
{
    if (!s_mounted) {
        printf("SD card not mounted\n");
        return;
    }

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        printf("Error: cannot open " SD_MOUNT_POINT "\n");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char path[260];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            printf("%-20s %ld\n", entry->d_name, (long)st.st_size);
        } else {
            printf("%-20s ???\n", entry->d_name);
        }
    }
    closedir(dir);
}

bool sdcard_delete_file(const char *filename)
{
    if (!s_mounted) {
        printf("SD card not mounted\n");
        return false;
    }

    char path[260];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", filename);

    if (remove(path) != 0) {
        printf("Error: could not delete %s\n", filename);
        return false;
    }
    return true;
}

void sdcard_print_info(void)
{
    if (!s_mounted) {
        printf("mounted: no\n");
        return;
    }
    printf("mounted: yes\n");

    /* Card info from sdmmc driver */
    if (s_card) {
        uint64_t card_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
        printf("card_capacity_sectors: %lu\n", (unsigned long)s_card->csd.capacity);
        printf("card_sector_size:      %u bytes\n", s_card->csd.sector_size);
        printf("card_total:            %llu bytes (%.2f GiB)\n",
               (unsigned long long)card_bytes,
               (double)card_bytes / (1024.0 * 1024.0 * 1024.0));
    }

    /* FATFS filesystem info */
    FATFS *fs;
    DWORD free_clust;
    FRESULT fr = f_getfree("0:", &free_clust, &fs);
    if (fr != FR_OK) {
        printf("f_getfree error: %d\n", (int)fr);
        return;
    }

    DWORD total_clust = fs->n_fatent - 2;
    uint32_t sect_per_clust = fs->csize;
    /* When FF_MAX_SS == FF_MIN_SS == 512 the ssize field does not exist */
    uint32_t bytes_per_sect = 512;

    uint64_t total_bytes = (uint64_t)total_clust * sect_per_clust * bytes_per_sect;
    uint64_t free_bytes  = (uint64_t)free_clust  * sect_per_clust * bytes_per_sect;

    printf("fat_total_clusters:    %lu\n", (unsigned long)total_clust);
    printf("fat_free_clusters:     %lu\n", (unsigned long)free_clust);
    printf("fat_sectors_per_clust: %lu\n", (unsigned long)sect_per_clust);
    printf("fat_bytes_per_sector:  %lu\n", (unsigned long)bytes_per_sect);
    printf("fat_total:             %llu bytes (%.2f GiB)\n",
           (unsigned long long)total_bytes,
           (double)total_bytes / (1024.0 * 1024.0 * 1024.0));
    printf("fat_free:              %llu bytes (%.2f GiB)\n",
           (unsigned long long)free_bytes,
           (double)free_bytes / (1024.0 * 1024.0 * 1024.0));
}

bool sdcard_format(void)
{
    if (!s_mounted) {
        printf("error: SD card not mounted\n");
        return false;
    }
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD format failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SD card formatted");
    return true;
}

bool sdcard_write_test(const char *filename, size_t nbytes)
{
    if (!s_mounted) {
        printf("SD card not mounted\n");
        return false;
    }

    char path[64];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", filename);

    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("Error: cannot create %s\n", filename);
        return false;
    }

    uint8_t buf[256];
    size_t written = 0;
    while (written < nbytes) {
        size_t chunk = nbytes - written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        for (size_t i = 0; i < chunk; i++) {
            buf[i] = (uint8_t)((written + i) & 0xFF);
        }

        size_t n = fwrite(buf, 1, chunk, f);
        if (n != chunk) {
            printf("Error: write failed at offset %zu\n", written);
            fclose(f);
            return false;
        }
        written += n;
    }

    fclose(f);
    printf("Wrote %zu bytes to %s\n", written, filename);
    return true;
}

#endif /* CONFIG_FORCE4_SD_CARD */
