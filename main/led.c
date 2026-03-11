#include "led.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#define LED_GPIO        21
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_DUTY_RES   LEDC_TIMER_13_BIT
#define LEDC_MAX_DUTY   8191
#define LEDC_FREQ_HZ    1000

void led_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num   = LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&channel);
}

void led_set(uint16_t duty)
{
    uint32_t d = duty > LEDC_MAX_DUTY ? LEDC_MAX_DUTY : duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, d);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

void led_off(void)
{
    led_set(0);
}

void led_idle_update(float t_s)
{
    float phase = fmodf(t_s, 2.0f);
    led_set(phase < 0.05f ? LEDC_MAX_DUTY : 0);
}

void led_flash_update(float t_s)
{
    int on = ((int)(t_s * 10.0f)) % 2;
    led_set(on ? LEDC_MAX_DUTY : 0);
}

void led_transfer_update(float t_s)
{
    // Double-blink every 2s: ON-OFF-ON-OFF(long)
    float phase = fmodf(t_s, 2.0f);
    int on = (phase < 0.12f) || (phase >= 0.25f && phase < 0.37f);
    led_set(on ? LEDC_MAX_DUTY : 0);
}

void led_blink_n(int count)
{
    for (int i = 0; i < count; i++) {
        led_set(LEDC_MAX_DUTY);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_set(0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
