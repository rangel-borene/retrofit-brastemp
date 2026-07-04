/*
 * SPDX-FileCopyrightText: 2024 Retrofit Brastemp Project
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "retrofit_brastemp";

#define BUTTON_GPIO 41
#define DEBOUNCE_MS CONFIG_DEBOUNCE_MS

#define NVS_NAMESPACE "storage"
#define NVS_KEY_STATE "button_state"

static uint8_t s_state = 0;             /* 0 = desligado, 1 = ligado */
static uint8_t s_last_button_state = 1; /* Pull-up, unpressed = 1 */
static nvs_handle_t s_nvs_handle;

static void configure_button(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err;

    /* Initialize NVS */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* NVS partition was truncated and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Open NVS handle */
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    /* Read saved state */
    uint8_t saved_state = 0;
    err = nvs_get_u8(s_nvs_handle, NVS_KEY_STATE, &saved_state);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Key not found — write default (0 = desligado) */
        ESP_LOGI(TAG, "No saved state found, defaulting to desligado");
        err = nvs_set_u8(s_nvs_handle, NVS_KEY_STATE, 0);
        ESP_ERROR_CHECK(err);
        err = nvs_commit(s_nvs_handle);
        ESP_ERROR_CHECK(err);
        s_state = 0;
    }
    else
    {
        ESP_ERROR_CHECK(err);
        s_state = saved_state;
    }

    ESP_LOGI(TAG, "Restored state: %s", s_state ? "ligado" : "desligado");
    return ESP_OK;
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
            s_state = !s_state;
            ESP_LOGI(TAG, "%s", s_state ? "ligado" : "desligado");

            /* Persist state to NVS */
            esp_err_t err = nvs_set_u8(s_nvs_handle, NVS_KEY_STATE, s_state);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save state: %s", esp_err_to_name(err));
            }
            else
            {
                err = nvs_commit(s_nvs_handle);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
                }
            }
        }
    }

    s_last_button_state = current_level;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Retrofit Brastemp - Button Monitor");
    ESP_LOGI(TAG, "Button GPIO: %d", BUTTON_GPIO);
    ESP_LOGI(TAG, "Debounce time: %d ms", DEBOUNCE_MS);

    configure_button();

    /* Initialize NVS and restore state */
    init_nvs();

    while (1)
    {
        debounce_and_toggle();
        vTaskDelay(pdMS_TO_TICKS(10)); /* Poll every 10ms */
    }
}
