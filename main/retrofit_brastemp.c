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
#include "preset_functions.h"

static const char *TAG = "retrofit_brastemp";

/* Sensor de pressão GPIO 4 - intervalo de impressão (ms) */
#define PRINT_GPIO4_INTERVAL_MS 2000

#define BUTTON_GPIO 41
#define BUTTON_PAUSE_PLAY_GPIO 42
#define DEBOUNCE_MS CONFIG_DEBOUNCE_MS

#define GPIO_SENSOR_TAMPA ((gpio_num_t)5) /* Sensor de Tampa             [Digital] */

static uint8_t s_cycle_running = 0; /* 1 = ciclo em andamento */
static bool s_start = false;

static void finalizar_ciclo(bool abortado)
{
    if (abortado)
    {
        /* Só esvazia se houver água no tanque */
        if (ler_pressao_adc_mv() > LIMIAR_TANQUE_VAZIO_MV)
        {
            ESP_LOGI(TAG, "Ciclo abortado pelo usuário! Esvaziando tanque...");
            esvaziar();
        }
        else
        {
            ESP_LOGI(TAG, "Ciclo abortado — tanque já vazio, pulando esvaziar()");
        }
        limpar_abort();
    }
    else
    {
        ESP_LOGI(TAG, "Ciclo Edredon concluido");
    }

    /* Garante que os LEDs estejam apagados e a pausa limpa */
    led_ciclo_rodando(false);

    /* Fecha o dreno caso tenha sido aberto pela task_iniciar_abortar */
    fechar_valvula_dreno();

    s_cycle_running = 0;
}

/**
 * @brief Bloqueia até que a pausa seja liberada.
 *
 * A flag de pausa é alterada por outra task (task_pause_play /
 * task_tampa_aberta), portanto a leitura via obter_pausa() precisa ser
 * feita a cada iteração — uma variável local "bool pausado" ficaria
 * obsoleta (stale) e causaria loop infinito.
 */
static void aguardar_pausa_liberada(void)
{
    while (obter_pausa())
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void executar_ciclo_edredon(void)
{
    ESP_LOGI(TAG, "Iniciando ciclo Edredon");

    /* Captura pontual do status (foto do instante) — use para decisões
     * imediatas, NÃO para loops de espera. */
    bool pausado = obter_pausa();
    if (pausado)
    {
        ESP_LOGI(TAG, "Ciclo iniciado com pausa ja ativa");
    }

    s_cycle_running = 1;
    led_ciclo_rodando(true);

    /* Etapa 1: Encher o tanque (200 ml de sabao para edredon) */
    ESP_LOGI(TAG, "Enchendo tanque - água quente, 200 ml de sabão, nível máximo");
    encher(AGUA_QUENTE, PRODUTO_1, 200, NIVEL_4);

    /* Aguarda possível pausa ser liberada antes de prosseguir */
    aguardar_pausa_liberada();

    /* Etapa 2: Bater/agitar por 15 minutos */
    ESP_LOGI(TAG, "Batendo roupa por 15 minutos...");
    bater(900); /* 15 minutos = 900 segundos */

    aguardar_pausa_liberada();

    /* Etapa 3: Esvaziar o tanque */
    ESP_LOGI(TAG, "Esvaziando tanque...");
    esvaziar();

    aguardar_pausa_liberada();

    /* Etapa 4: Centrifugar por 5 minutos */
    ESP_LOGI(TAG, "Centrifugando por 5 minutos...");
    centrifugar(300); /* 5 minutos = 300 segundos */

    finalizar_ciclo(obter_abort());
}

/**
 * @brief Task dedicada ao monitoramento da tampa.
 *
 *   - Quando a tampa ABRE (borda de descida 1→0): equivalente a pressionar o
 *     botão pause.
 *   - Quando a tampa FECHA (borda de subida 0→1): apenas informa via log que
 *     o usuário deve clicar no botão Continue (pause/play) para prosseguir.
 *
 * A reativação do ciclo é feita exclusivamente pelo botão Pausar/Play (task_pause_play).
 */
static void task_tampa_aberta(void *pvParameters)
{
    (void)pvParameters;
    uint8_t last_level = 1; /* Pull-up: não pressionado = 1 (tampa fechada) */

    while (1)
    {
        uint8_t current_level = gpio_get_level(GPIO_SENSOR_TAMPA);

        /* Borda de descida: 1 → 0 (tampa ABRIU) */
        if (last_level == 1 && current_level == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            if (gpio_get_level(GPIO_SENSOR_TAMPA) == 0)
            {
                if (s_cycle_running)
                {
                    ESP_LOGW(TAG, "[SEGURANCA] Tampa aberta! Acionando pausar");
                    pausar();
                }
                else
                {
                    ESP_LOGI(TAG, "[SEGURANCA] Tampa aberta. (fora de ciclo)");
                }
            }
        }

        /* Borda de subida: 0 → 1 (tampa FECHOU) — apenas informa */
        if (last_level == 0 && current_level == 1)
        {
            if (gpio_get_level(GPIO_SENSOR_TAMPA) == 1)
            {
                if (s_cycle_running && obter_pausa())
                {
                    ESP_LOGI(TAG, "[SEGURANCA] Tampa fechada. Clique no botao Continue para prosseguir.");
                }
                else
                {
                    ESP_LOGI(TAG, "[SEGURANCA] Tampa fechada. (fora de ciclo)");
                }
            }
        }

        last_level = current_level;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Task dedicada ao botão Iniciar/Abortar.
 *
 * Substitui a antiga debounce_and_toggle() para GPIO 41.
 * Funciona como a task_pause_play para o botão Iniciar/Abortar:
 *
 * - Se um ciclo está RODANDO e o botão é pressionado → ABORTA:
 *      para o motor imediatamente, abre o dreno e seta a flag de abort.
 *
 * - Se NENHUM ciclo está rodando e o botão é pressionado → INICIA:
 *      alimenta a flag iniciar() que o loop principal consome.
 *
 * Roda em paralelo para garantir resposta imediata mesmo se a main loop
 * estiver bloqueada em operações longas (ex: encher, bater, centrifugar).
 */
static void task_iniciar_abortar(void *pvParameters)
{
    (void)pvParameters;
    uint8_t last_level = 1; /* Pull-up: não pressionado = 1 */

    while (1)
    {
        uint8_t current_level = gpio_get_level(BUTTON_GPIO);

        /* Detecta falling edge (pressionou) com pull-up: 1 -> 0 */
        if (last_level == 1 && current_level == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            if (gpio_get_level(BUTTON_GPIO) == 0)
            {
                if (s_cycle_running)
                {
                    /* --- ABORT: ciclo em andamento --- */
                    ESP_LOGW(TAG, "[ABORT] Botao Iniciar/Abortar pressionado durante ciclo!");

                    /* Ação imediata de segurança: parar motor e abrir dreno */
                    parar_motor_seguro();
                    abrir_valvula_dreno();

                    /* Seta a flag de abort para o ciclo */
                    solicitar_abort();
                }
                else
                {
                    /* --- INICIAR: fora de ciclo --- */
                    ESP_LOGI(TAG, "[INICIAR] Botao Iniciar/Abortar pressionado fora de ciclo.");

                    /* Alimenta a flag que o loop principal consome */
                    s_start = true;
                }
            }
        }

        /* Só aceita nova borda de descida quando o botão for solto */
        last_level = current_level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief Task dedicada ao botão Pausar/Play.
 *
 * Roda em paralelo com app_main para permitir pausar/continuar mesmo
 * quando o ciclo está bloqueando a task principal.
 */
static void task_pause_play(void *pvParameters)
{
    (void)pvParameters;
    uint8_t last_level = 1;

    while (1)
    {
        uint8_t current_level = gpio_get_level(BUTTON_PAUSE_PLAY_GPIO);

        /* Detecta falling edge (pressionou) com pull-up: 1 -> 0 */
        if (last_level == 1 && current_level == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            if (gpio_get_level(BUTTON_PAUSE_PLAY_GPIO) == 0)
            {
                if (s_cycle_running)
                {
                    if (obter_pausa())
                    {
                        continuar();
                    }
                    else
                    {
                        pausar();
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Botao Pausar/Play pressionado fora de um ciclo");
                }
            }
        }

        last_level = current_level;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Retrofit Brastemp - V 6.0");
    ESP_LOGI(TAG, "Button Iniciar/Abortar GPIO: %d", BUTTON_GPIO);
    ESP_LOGI(TAG, "Button Pausar/Play GPIO: %d", BUTTON_PAUSE_PLAY_GPIO);
    ESP_LOGI(TAG, "Debounce time: %d ms", DEBOUNCE_MS);

    /* Configura os GPIOs dos botões (redundante com configurar_gpios_preset) */
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    gpio_reset_pin(BUTTON_PAUSE_PLAY_GPIO);
    gpio_set_direction(BUTTON_PAUSE_PLAY_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PAUSE_PLAY_GPIO, GPIO_PULLUP_ONLY);

    gpio_reset_pin(GPIO_SENSOR_TAMPA);
    gpio_set_direction(GPIO_SENSOR_TAMPA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_SENSOR_TAMPA, GPIO_PULLUP_ONLY);

    /* Initialize preset function GPIOs */
    configurar_gpios_preset();

    /* Cria tasks monitoras (em paralelo, sempre rodando) */
    xTaskCreate(task_tampa_aberta, "tampa_aberta", 2048, NULL, 6, NULL);
    xTaskCreate(task_iniciar_abortar, "iniciar_abortar", 2048, NULL, 5, NULL);
    xTaskCreate(task_pause_play, "pause_play", 2048, NULL, 5, NULL);

    while (1)
    {
        /* Verifica se o botão Iniciar foi pressionado (fora de ciclo) */
        if (s_start && !s_cycle_running)
        {
            s_start = false;
            executar_ciclo_edredon();
        }

        // /* Imprime o valor do GPIO 4 (sensor de pressao ADC) periodicamente */
        // uint32_t now = xTaskGetTickCount();
        // if ((now - last_print_tick) >= pdMS_TO_TICKS(PRINT_GPIO4_INTERVAL_MS))
        // {
        //     uint32_t tensao_mv = ler_pressao_adc_mv();
        //     ESP_LOGI(TAG, "GPIO4 (sensor pressao): %" PRIu32 " mV", tensao_mv);
        //     last_print_tick = now;
        // }

        vTaskDelay(pdMS_TO_TICKS(10)); /* Poll every 10ms */
    }
}