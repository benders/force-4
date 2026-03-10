#include "serial_cmd.h"
#include "storage.h"
#include "flight_logger.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "serial_cmd";

static bool flight_mode = true;

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

static void cmd_cat(const char *filename)
{
    if (!filename || *filename == '\0') {
        printf("Usage: cat <filename>\n");
        return;
    }

    char path[48];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Error: cannot open %s\n", filename);
        return;
    }

    char line[128];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stdout);
        if (++count % 256 == 0) {
            vTaskDelay(1); /* yield to IDLE so it can reset its WDT subscription */
        }
    }
    fclose(f);
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
    printf("  cat <filename>  Print file contents\n");
    printf("  rm <filename>   Delete file (requires transfer state)\n");
    printf("  status          Show system status\n");
    printf("  transfer        Pause logging; show transfer LED pattern\n");
    printf("  resume          Exit transfer mode; return to IDLE\n");
    printf("  ping            Connection test\n");
    printf("  help            Show this help\n");
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

    printf("---BEGIN---\n");

    if (strcmp(line, "ls") == 0) {
        cmd_ls();
    } else if (strcmp(line, "cat") == 0) {
        cmd_cat(arg);
    } else if (strcmp(line, "rm") == 0) {
        cmd_rm(arg);
    } else if (strcmp(line, "status") == 0) {
        cmd_status();
    } else if (strcmp(line, "transfer") == 0) {
        flight_logger_enter_transfer();
        printf("ok\n");
    } else if (strcmp(line, "resume") == 0) {
        flight_logger_exit_transfer();
        printf("ok\n");
    } else if (strcmp(line, "ping") == 0) {
        printf("pong\n");
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
