/*
 * SPDX-FileCopyrightText: 2024 Retrofit Brastemp Project
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "retrofit_brastemp";

#define RELAY_GPIO CONFIG_RELAY_GPIO
#define BUTTON_GPIO CONFIG_BUTTON_GPIO
#define DEBOUNCE_MS CONFIG_DEBOUNCE_MS

static uint8_t s_relay_state = 0;
static uint8_t s_last_button_state = 1; /* Assumes pull-up, so unpressed = 1 */

static void configure_relay(void)
{
    gpio_reset_pin(RELAY_GPIO);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_GPIO, 0);
}

static void configure_button(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
}

static void toggle_relay(void)
{
    s_relay_state = !s_relay_state;
    gpio_set_level(RELAY_GPIO, s_relay_state);
    ESP_LOGI(TAG, "Relay %s", s_relay_state ? "ON" : "OFF");
}

static void debounce_and_toggle(void)
{
    uint8_t current_level = gpio_get_level(BUTTON_GPIO);

    /* Detects falling edge (press) with pull-up: 1 -> 0 */
    if (s_last_button_state == 1 && current_level == 0)
    {
        /* Wait debounce time and check again */
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        if (gpio_get_level(BUTTON_GPIO) == 0)
        {
            toggle_relay();
        }
    }

    s_last_button_state = current_level;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Retrofit Brastemp - Relay Control");
    ESP_LOGI(TAG, "Relay GPIO: %d", RELAY_GPIO);
    ESP_LOGI(TAG, "Button GPIO: %d", BUTTON_GPIO);
    ESP_LOGI(TAG, "Debounce time: %d ms", DEBOUNCE_MS);

    configure_relay();
    configure_button();

    while (1)
    {
        debounce_and_toggle();
        vTaskDelay(pdMS_TO_TICKS(10)); /* Poll every 10ms */
    }
}