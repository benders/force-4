#pragma once

typedef enum {
    FLIGHT_STATE_IDLE,
    FLIGHT_STATE_LOGGING,
    FLIGHT_STATE_COOLDOWN,
    FLIGHT_STATE_TRANSFER,  // data transfer in progress, logging paused
} flight_state_t;

/**
 * Get current flight logger state.
 */
flight_state_t flight_logger_get_state(void);

/**
 * Get total number of flights recorded this session.
 */
int flight_logger_get_flight_count(void);

/**
 * Pause flight logging and enter data-transfer mode.
 * Call from serial_cmd when the host data script connects.
 */
void flight_logger_enter_transfer(void);

/**
 * Exit data-transfer mode and return to IDLE (flight ready).
 * Safe to call from any task; the actual transition happens in flight_task.
 * Also triggered automatically after TRANSFER_TIMEOUT_S seconds.
 */
void flight_logger_exit_transfer(void);

/**
 * Flight logger FreeRTOS task. Pin to core 1.
 * pvParameters unused.
 */
void flight_task(void *pvParameters);

/**
 * SPIFFS write task. Pin to core 0. Drains the log ring buffer to the
 * flight file so flight_task never blocks on flash I/O.
 * pvParameters unused.
 */
void log_write_task(void *pvParameters);
