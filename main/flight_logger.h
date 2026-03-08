#pragma once

typedef enum {
    FLIGHT_STATE_IDLE,
    FLIGHT_STATE_LOGGING,
    FLIGHT_STATE_COOLDOWN,
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
 * Flight logger FreeRTOS task. Pin to core 1.
 * pvParameters unused.
 */
void flight_task(void *pvParameters);
