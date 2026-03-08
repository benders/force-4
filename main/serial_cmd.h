#pragma once

#include <stdbool.h>

/**
 * Serial command handler FreeRTOS task.
 * pvParameters: bool* pointing to flight_mode flag.
 */
void serial_cmd_task(void *pvParameters);
