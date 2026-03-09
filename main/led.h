#pragma once

#include <stdint.h>

void led_init(void);
void led_breathe_update(float t_s);
void led_flash_update(float t_s);
void led_transfer_update(float t_s);  // double-blink every 2s (data transfer)
void led_blink_n(int count);
void led_off(void);
void led_set(uint16_t duty);
