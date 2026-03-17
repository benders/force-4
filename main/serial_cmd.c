#include "serial_cmd.h"
#include "storage.h"
#include "flight_logger.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef CONFIG_FORCE4_SD_CARD
#include "sdcard.h"
#endif
#ifdef CONFIG_FORCE4_CAMERA
#include "camera.h"
#endif

static const char *TAG = "serial_cmd";

static bool flight_mode = true;

/* Pending binary transfer state (set by 'cat', consumed by 'go'). */
static char s_pending_path[64] = {0};
static long s_pending_size     = -1;  /* -1 = no pending transfer */

static const char *state_name(flight_state_t s)
{
    switch (s) {
    case FLIGHT_STATE_IDLE:     return "FLIGHT_STATE_IDLE";
    case FLIGHT_STATE_LOGGING:  return "FLIGHT_STATE_LOGGING";
    case FLIGHT_STATE_COOLDOWN: return "FLIGHT_STATE_COOLDOWN";
    case FLIGHT_STATE_TRANSFER: return "FLIGHT_STATE_TRANSFER";
    default:                    return "unknown";
    }
}

static void cmd_ls(void)
{
    storage_list_flights();
}

/* Prepare a binary transfer: stat the file, store path+size, reply "ready size:N".
 * The actual bytes are sent only when the host sends 'go'. */
static void cmd_cat_prepare(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        const char *name = strrchr(path, '/');
        printf("error: cannot stat %s\n", name ? name + 1 : path);
        return;
    }
    strncpy(s_pending_path, path, sizeof(s_pending_path) - 1);
    s_pending_path[sizeof(s_pending_path) - 1] = '\0';
    s_pending_size = (long)st.st_size;
    printf("ready size:%ld\n", s_pending_size);
}

static void cmd_cat(const char *filename)
{
    if (!filename || *filename == '\0') {
        printf("Usage: cat <filename>\n");
        return;
    }
    char path[48];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);
    cmd_cat_prepare(path);
}

/* Send the pending binary transfer.  Called BEFORE ---BEGIN--- (no framing). */
static void cmd_go(void)
{
    if (s_pending_size < 0) {
        ESP_LOGE(TAG, "go: no pending transfer");
        return;
    }
    FILE *f = fopen(s_pending_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "go: cannot open %s", s_pending_path);
        s_pending_size = -1;
        s_pending_path[0] = '\0';
        return;
    }
    s_pending_size = -1;
    s_pending_path[0] = '\0';

    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    uint8_t buf[512];
    size_t n;
    int chunk = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
        if (++chunk % 8 == 0) {
            vTaskDelay(1); /* yield so IDLE task can reset its WDT subscription */
        }
    }
    fclose(f);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
}

static void cmd_abort(void)
{
    s_pending_size = -1;
    s_pending_path[0] = '\0';
    printf("ok\n");
}

static void cmd_rm(const char *filename)
{
    if (flight_mode && flight_logger_get_state() != FLIGHT_STATE_TRANSFER) {
        printf("denied: flight mode (send 'transfer' first)\n");
        return;
    }
    if (!filename || *filename == '\0') {
        printf("Usage: rm <filename>\n");
        return;
    }
    if (storage_delete_file(filename)) {
        printf("Deleted %s\n", filename);
    } else {
        printf("Error: could not delete %s\n", filename);
    }
}

static void cmd_status(void)
{
    printf("mode: %s\n", flight_mode ? "flight" : "data");
    printf("state: %s\n", state_name(flight_logger_get_state()));
    printf("free: %zu bytes\n", storage_free_space());
    printf("flights: %d\n", flight_logger_get_flight_count());
    printf("uptime: %lld s\n", (long long)(esp_timer_get_time() / 1000000LL));
}

static void cmd_help(void)
{
    printf("Commands:\n");
    printf("  ls              List flight files\n");
    printf("  cat <filename>  Prepare file for transfer (follow with 'go')\n");
    printf("  rm <filename>   Delete file (requires transfer state)\n");
    printf("  status          Show system status\n");
    printf("  trigger         Manually start a flight recording (IDLE only)\n");
    printf("  transfer        Pause logging; show transfer LED pattern\n");
    printf("  resume          Exit transfer mode; return to IDLE\n");
    printf("  ping            Connection test\n");
    printf("  go              Send pending binary transfer (after cat)\n");
    printf("  abort           Cancel pending binary transfer\n");
    printf("  help            Show this help\n");
#ifdef CONFIG_FORCE4_SD_CARD
    printf("SD card commands:\n");
    printf("  ls --sd         List SD card files\n");
    printf("  cat --sd <file> Prepare SD file for transfer (follow with 'go')\n");
    printf("  rm --sd <file>  Delete SD card file\n");
    printf("  sdtest [N]      Write N-byte test pattern to SD (default 1024)\n");
    printf("  sdinfo          Show SD card status and free space\n");
#endif
#ifdef CONFIG_FORCE4_CAMERA
    printf("Camera commands:\n");
    printf("  photo           Capture JPEG and save to /sd/PHOTO.JPG\n");
#endif
}

static void process_line(char *line)
{
    // Trim trailing whitespace
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    // Split command and argument
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
    }

    // Parse --sd flag from argument
    bool use_sd = false;
#ifdef CONFIG_FORCE4_SD_CARD
    if (arg && strncmp(arg, "--sd", 4) == 0) {
        use_sd = true;
        arg += 4;
        while (*arg == ' ') arg++;
        if (*arg == '\0') arg = NULL;
    }
#endif

    /* 'go' sends raw binary with no ---BEGIN---/---END--- framing.
     * It must be handled before the framing printf and return early. */
    if (strcmp(line, "go") == 0) {
        cmd_go();
        return;
    }

    printf("---BEGIN---\n");

    if (strcmp(line, "ls") == 0) {
#ifdef CONFIG_FORCE4_SD_CARD
        if (use_sd) { sdcard_list_files(); }
        else
#endif
        { (void)use_sd; cmd_ls(); }
    } else if (strcmp(line, "cat") == 0) {
#ifdef CONFIG_FORCE4_SD_CARD
        if (use_sd) {
            if (!arg || *arg == '\0') {
                printf("Usage: cat --sd <filename>\n");
            } else if (!sdcard_is_mounted()) {
                printf("error: SD card not mounted\n");
            } else {
                char path[64];
                snprintf(path, sizeof(path), "/sd/%s", arg);
                cmd_cat_prepare(path);
            }
        } else
#endif
        { cmd_cat(arg); }
    } else if (strcmp(line, "abort") == 0) {
        cmd_abort();
    } else if (strcmp(line, "rm") == 0) {
#ifdef CONFIG_FORCE4_SD_CARD
        if (use_sd) {
            if (!arg || *arg == '\0') {
                printf("Usage: rm --sd <filename>\n");
            } else if (sdcard_delete_file(arg)) {
                printf("Deleted %s\n", arg);
            } else {
                printf("Error: could not delete %s\n", arg);
            }
        } else
#endif
        { cmd_rm(arg); }
    } else if (strcmp(line, "status") == 0) {
        cmd_status();
    } else if (strcmp(line, "trigger") == 0) {
        if (flight_logger_get_state() != FLIGHT_STATE_IDLE) {
            printf("denied: only allowed in IDLE state\n");
        } else {
            flight_logger_trigger();
            printf("ok\n");
        }
    } else if (strcmp(line, "transfer") == 0) {
        flight_logger_enter_transfer();
        printf("ok\n");
    } else if (strcmp(line, "resume") == 0) {
        flight_logger_exit_transfer();
        printf("ok\n");
    } else if (strcmp(line, "ping") == 0) {
        printf("pong\n");
#ifdef CONFIG_FORCE4_SD_CARD
    } else if (strcmp(line, "sdtest") == 0) {
        size_t nbytes = 1024;
        if (arg && *arg != '\0') {
            nbytes = (size_t)atoi(arg);
            if (nbytes == 0) nbytes = 1024;
        }
        if (sdcard_write_test("test_sd", nbytes)) {
            printf("ok\n");
        }
    } else if (strcmp(line, "sdinfo") == 0) {
        sdcard_print_info();
#endif
#ifdef CONFIG_FORCE4_CAMERA
    } else if (strcmp(line, "photo") == 0) {
        if (!sdcard_is_mounted()) {
            printf("error: SD card not mounted\n");
        } else if (!camera_capture_to_sd("/sd/PHOTO.JPG")) {
            printf("error: capture failed\n");
        } else {
            struct stat st;
            if (stat("/sd/PHOTO.JPG", &st) == 0) {
                printf("ok size:%ld\n", (long)st.st_size);
            } else {
                printf("error: stat failed\n");
            }
        }
#endif
    } else if (strcmp(line, "help") == 0) {
        cmd_help();
    } else {
        printf("Unknown command: %s (type 'help')\n", line);
    }

    printf("---END---\n");
}

void serial_cmd_task(void *pvParameters)
{
    if (pvParameters) {
        flight_mode = *(bool *)pvParameters;
    }

    ESP_LOGI(TAG, "Serial command handler started (mode=%s)",
             flight_mode ? "flight" : "data");

    // Disable stdio buffering on stdout so printf output is sent immediately.
    // ESP_LOGI bypasses stdio (always unbuffered), but plain printf() is not.
    setvbuf(stdout, NULL, _IONBF, 0);

    // Machine-parseable ready marker (useful for manual monitoring)
    printf("FORCE4:READY\n");
    fflush(stdout);

    char line[128];
    int pos = 0;

    while (1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                ESP_LOGI(TAG, "cmd: %s", line);
                process_line(line);
                fflush(stdout);
                pos = 0;
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)c;
        }
    }
}
