/*
 * SPDX-FileCopyrightText: 2024 Retrofit Brastemp Project
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Implementação das funções base para presets da máquina de lavar.
 */

#include "preset_functions.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "preset_functions";

/* ------------------------------------------------------------------ */
/*  Flag de abort compartilhada                                        */
/* ------------------------------------------------------------------ */

static volatile bool s_abort = false;

bool obter_abort(void)
{
    return s_abort;
}

void solicitar_abort(void)
{
    ESP_LOGI(TAG, "ABORT solicitado!");
    s_abort = true;
}

void limpar_abort(void)
{
    s_abort = false;
}

/* ------------------------------------------------------------------ */
/*  Definições dos GPIOs                                              */
/* ------------------------------------------------------------------ */

/* Sensor de pressão (ADC) para monitorar nível de água */
#define GPIO_SENSOR_PRESSAO ((gpio_num_t)4) /* QDW90A 0-0.5bar, saída 0-3.3V */
#define ADC_CHANNEL_SENSOR ADC_CHANNEL_3    /* GPIO 4 = ADC1_CH3 no ESP32-S3 */

/* Sensores de entrada (digitais) */
#define GPIO_SENSOR_TAMPA ((gpio_num_t)5)            /* Sensor de Tampa             [Digital] */
#define GPIO_SENSOR_DESBALANCEAMENTO ((gpio_num_t)6) /* Sensor de Desbalanceamento  [Digital] */
#define GPIO_DETECTOR_FALTA_ENERGIA ((gpio_num_t)40) /* Detector de Falta de Energia [Digital] ⚠️ */
#define GPIO_BOTAO_INICIAR ((gpio_num_t)41)          /* Botão Iniciar/Abortar       [Digital] */
#define GPIO_BOTAO_PAUSAR_PLAY ((gpio_num_t)42)      /* Botão Pausar/Play           [Digital] */

/* Válvulas de água */
#define GPIO_VALVULA_AGUA_FRIA ((gpio_num_t)13)   /* Válvula Solenoide - Água Fria   */
#define GPIO_VALVULA_AGUA_QUENTE ((gpio_num_t)15) /* Válvula Solenoide - Água Quente */

/* Motor (SSR Mestre + relés de direção) */
#define GPIO_SSR_MESTRE ((gpio_num_t)18)    /* SSR Mestre (liga/desliga motor) */
#define GPIO_MOTOR_HORARIO ((gpio_num_t)14) /* Relé Horário                    */
#define GPIO_MOTOR_ANTI_H ((gpio_num_t)12)  /* Relé Anti-Horário               */

/* Bomba de drenagem */
#define GPIO_BOMBA_DREENO ((gpio_num_t)9) /* Bomba de Drenagem */

/* Dosadores de produto químico (Kamoer KPHM400 - 400ml/min) */
#define GPIO_DOSADOR_1 ((gpio_num_t)48) /* Bomba Detergente    */
#define GPIO_DOSADOR_2 ((gpio_num_t)47) /* Bomba Clarificante  */
#define GPIO_DOSADOR_3 ((gpio_num_t)39) /* Bomba Neutralizante */
#define GPIO_DOSADOR_4 ((gpio_num_t)46) /* Bomba Amaciante */

/* LED de alerta */
#define GPIO_LED_ALERTA ((gpio_num_t)21) /* LED de Alerta */

/* ------------------------------------------------------------------ */
/*  Constantes de calibração (ajustar manualmente)                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Limiar de tensão para considerar o tanque vazio.
 *
 * CALIBRAR MANUALMENTE:
 * 1. Deixe o tanque completamente vazio e seco.
 * 2. Leia o valor do sensor no monitor serial.
 * 3. Adicione uma margem de 10-20 mV.
 * 4. Atualize este valor.
 */
#define LIMIAR_TANQUE_VAZIO_MV 25 // Exemplo: 10mV lido + 15mV margem

/**
 * @brief Mapeamento de níveis de água para tensão do sensor.
 *        Sensor QDW90A: 0V = vazio, 3.3V = 0.5 bar (~5 metros de água).
 */
#define NIVEL_1_MV 825  // 25% do nível máximo
#define NIVEL_2_MV 1650 // 50% do nível máximo
#define NIVEL_3_MV 2475 // 75% do nível máximo
#define NIVEL_4_MV 3300 // 100% do nível máximo

/* ------------------------------------------------------------------ */
/*  Helpers internos                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Lê a tensão do sensor de pressão (GPIO 4) via ADC.
 *        Sensor QDW90A 0-0.5bar, saída 0-3.3V.
 *        0V = vazio, 3.3V = nível máximo (0.5 bar).
 *
 * @return Tensão em mV (0 a 3300).
 */
static uint32_t ler_tensao_adc(void)
{
    static adc_oneshot_unit_handle_t adc_handle = NULL;
    static adc_cali_handle_t cali_handle = NULL;
    static int inicializado = 0;

    if (!inicializado)
    {
        /* Configura pull-down interno no GPIO 4 para estabilizar o sinal */
        gpio_set_pull_mode(GPIO_SENSOR_PRESSAO, GPIO_PULLDOWN_ONLY);

        /* Configura ADC oneshot */
        adc_oneshot_unit_init_cfg_t adc_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &adc_handle));

        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_12,
            .atten = ADC_ATTEN_DB_12, /* 0-3.3V */
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_SENSOR, &chan_cfg));

        /* Calibração (curve fitting para ESP32-S3) */
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = ADC_CHANNEL_SENSOR,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Calibração ADC indisponível, usando raw sem calibração");
            cali_handle = NULL;
        }

        inicializado = 1;
    }

    int raw;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_SENSOR, &raw));

    int tensao_mv = 0;
    if (cali_handle != NULL)
    {
        adc_cali_raw_to_voltage(cali_handle, raw, &tensao_mv);
    }
    else
    {
        /* Sem calibração: raw * 3300 / 4095 */
        tensao_mv = raw * 3300 / 4095;
    }

    return (uint32_t)tensao_mv;
}

/**
 * @brief Retorna a tensão correspondente ao nível desejado.
 */
static uint32_t obter_limiar_nivel(nivel_agua_t nivel)
{
    switch (nivel)
    {
    case NIVEL_1:
        return NIVEL_1_MV;
    case NIVEL_2:
        return NIVEL_2_MV;
    case NIVEL_3:
        return NIVEL_3_MV;
    case NIVEL_4:
        return NIVEL_4_MV;
    default:
        return NIVEL_4_MV;
    }
}

/**
 * @brief Retorna se o nível de água atual já atingiu o desejado.
 *
 * @param nivel_desejado  Nível desejado (NIVEL_1 a NIVEL_4).
 * @return true  se o nível medido >= desejado.
 * @return false se ainda está abaixo.
 */
static bool nivel_atingido(nivel_agua_t nivel_desejado)
{
    uint32_t tensao = ler_tensao_adc();
    uint32_t limiar_mv = obter_limiar_nivel(nivel_desejado);

    ESP_LOGD(TAG, "Sensor: %u mV, limiar nivel %d: %u mV", tensao, (int)nivel_desejado, limiar_mv);

    return (tensao >= limiar_mv);
}

/* ------------------------------------------------------------------ */
/*  Verificação de abort (lê GPIO diretamente nos ciclos longos)      */
/* ------------------------------------------------------------------ */

/**
 * @brief Lê o GPIO do botão (GPIO_BOTAO_INICIAR = 41) com debounce
 *        e seta s_abort = true se detectar falling edge (1 → 0).
 *
 * Deve ser chamada dentro dos loops de espera das funções de ciclo
 * (encher, bater, centrifugar, esvaziar) para permitir abort mesmo
 * quando o loop principal está bloqueado.
 */
static void verificar_abort(void)
{
    static uint8_t ultimo_nivel = 1;      /* Pull-up, não pressionado = 1 */
    static TickType_t tick_falling = 0;   /* Timestamp do falling edge */
    static uint8_t debounce_pendente = 0; /* Flag: aguardando confirmação do debounce */
    uint8_t nivel_atual = gpio_get_level(GPIO_BOTAO_INICIAR);

    if (!debounce_pendente)
    {
        /* Detecta falling edge: 1 → 0 (pressionou o botão) */
        if (ultimo_nivel == 1 && nivel_atual == 0)
        {
            tick_falling = xTaskGetTickCount();
            debounce_pendente = 1;
        }
    }
    else
    {
        /* Período de debounce transcorrido? */
        if ((xTaskGetTickCount() - tick_falling) >= pdMS_TO_TICKS(CONFIG_DEBOUNCE_MS))
        {
            /* Confirma se ainda está pressionado */
            if (gpio_get_level(GPIO_BOTAO_INICIAR) == 0)
            {
                ESP_LOGI(TAG, "ABORT detectado via GPIO (verificar_abort)!");
                s_abort = true;
            }
            debounce_pendente = 0;
        }
        else if (nivel_atual == 1)
        {
            /* Botão foi solto antes do fim do debounce — falso disparo */
            debounce_pendente = 0;
        }
    }

    ultimo_nivel = nivel_atual;
}

/* ------------------------------------------------------------------ */
/*  Funções públicas                                                   */
/* ------------------------------------------------------------------ */

uint32_t ler_pressao_adc_mv(void)
{
    return ler_tensao_adc();
}

void configurar_gpios_preset(void)
{
    /* --- SAÍDAS --- */

    /* Válvulas de água */
    gpio_reset_pin(GPIO_VALVULA_AGUA_FRIA);
    gpio_set_direction(GPIO_VALVULA_AGUA_FRIA, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_VALVULA_AGUA_FRIA, 0);

    gpio_reset_pin(GPIO_VALVULA_AGUA_QUENTE);
    gpio_set_direction(GPIO_VALVULA_AGUA_QUENTE, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_VALVULA_AGUA_QUENTE, 0);

    /* Dosadores */
    const gpio_num_t dosadores[] = {
        GPIO_DOSADOR_1,
        GPIO_DOSADOR_2,
        GPIO_DOSADOR_3,
        GPIO_DOSADOR_4,
    };
    for (int i = 0; i < 4; i++)
    {
        gpio_reset_pin(dosadores[i]);
        gpio_set_direction(dosadores[i], GPIO_MODE_OUTPUT);
        gpio_set_level(dosadores[i], 0);
    }

    /* Motor - SSR Mestre */
    gpio_reset_pin(GPIO_SSR_MESTRE);
    gpio_set_direction(GPIO_SSR_MESTRE, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SSR_MESTRE, 0);

    /* Motor - Relé Horário */
    gpio_reset_pin(GPIO_MOTOR_HORARIO);
    gpio_set_direction(GPIO_MOTOR_HORARIO, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_MOTOR_HORARIO, 0);

    /* Motor - Relé Anti-Horário */
    gpio_reset_pin(GPIO_MOTOR_ANTI_H);
    gpio_set_direction(GPIO_MOTOR_ANTI_H, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);

    /* Bomba de drenagem */
    gpio_reset_pin(GPIO_BOMBA_DREENO);
    gpio_set_direction(GPIO_BOMBA_DREENO, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_BOMBA_DREENO, 0);

    /* LED de alerta */
    gpio_reset_pin(GPIO_LED_ALERTA);
    gpio_set_direction(GPIO_LED_ALERTA, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_LED_ALERTA, 0);

    /* --- ENTRADAS --- */

    /* Sensor de tampa (entrada com pull-up) */
    gpio_reset_pin(GPIO_SENSOR_TAMPA);
    gpio_set_direction(GPIO_SENSOR_TAMPA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_SENSOR_TAMPA, GPIO_PULLUP_ONLY);

    /* Sensor de desbalanceamento */
    gpio_reset_pin(GPIO_SENSOR_DESBALANCEAMENTO);
    gpio_set_direction(GPIO_SENSOR_DESBALANCEAMENTO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_SENSOR_DESBALANCEAMENTO, GPIO_PULLUP_ONLY);

    /* Detector de falta de energia */
    gpio_reset_pin(GPIO_DETECTOR_FALTA_ENERGIA);
    gpio_set_direction(GPIO_DETECTOR_FALTA_ENERGIA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_DETECTOR_FALTA_ENERGIA, GPIO_PULLUP_ONLY);

    /* Botão Iniciar/Abortar */
    gpio_reset_pin(GPIO_BOTAO_INICIAR);
    gpio_set_direction(GPIO_BOTAO_INICIAR, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BOTAO_INICIAR, GPIO_PULLUP_ONLY);

    /* Botão Pausar/Play */
    gpio_reset_pin(GPIO_BOTAO_PAUSAR_PLAY);
    gpio_set_direction(GPIO_BOTAO_PAUSAR_PLAY, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BOTAO_PAUSAR_PLAY, GPIO_PULLUP_ONLY);
}

void encher(agua_t agua, produto_quimico_t produto, uint16_t quantidade_ml, nivel_agua_t nivel)
{
    ESP_LOGI(TAG, ">>> encher(agua=%s, produto=%d, quantidade_ml=%u, nivel=%d)",
             agua == AGUA_QUENTE ? "QUENTE" : "FRIA",
             (int)produto, quantidade_ml, (int)nivel);

    // --- 1. Abre a válvula de água ---
    gpio_num_t valvula_pin = (agua == AGUA_QUENTE) ? GPIO_VALVULA_AGUA_QUENTE
                                                   : GPIO_VALVULA_AGUA_FRIA;
    gpio_set_level(valvula_pin, 1);

    // --- 2. Aguarda atingir 50% do nível para dosagem ---
    uint32_t limiar_alvo = obter_limiar_nivel(nivel);
    uint32_t limiar_metade = limiar_alvo / 2;

    ESP_LOGI(TAG, "Aguardando 50%% do nível (%u mV)...", limiar_metade);
    while (ler_tensao_adc() < limiar_metade)
    {
        verificar_abort();
        if (s_abort)
        {
            ESP_LOGW(TAG, "ABORT durante espera de nível! Fechando válvula.");
            gpio_set_level(valvula_pin, 0);
            ESP_LOGI(TAG, "<<< encher abortado");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // --- 3. Dosagem do produto químico ---
    gpio_num_t dosador_pin = GPIO_NUM_NC; // Inicializa com valor seguro (Not Connected)
    uint8_t produto_valido = 1;
    switch (produto)
    {
    case PRODUTO_NENHUM:
        ESP_LOGI(TAG, "Nenhum produto químico selecionado");
        produto_valido = 0;
        break;
    case PRODUTO_1:
        dosador_pin = GPIO_DOSADOR_1;
        break;
    case PRODUTO_2:
        dosador_pin = GPIO_DOSADOR_2;
        break;
    case PRODUTO_3:
        dosador_pin = GPIO_DOSADOR_3;
        break;
    case PRODUTO_4:
        dosador_pin = GPIO_DOSADOR_4;
        break;
    default:
        ESP_LOGW(TAG, "Produto químico inválido: %d", (int)produto);
        produto_valido = 0;
        break;
    }

    if (produto_valido && quantidade_ml > 0)
    {
        uint32_t tempo_bomba_ms = (uint32_t)quantidade_ml * 150u;
        ESP_LOGI(TAG, "Dosando %u ml -> bomba ligada por %u ms", quantidade_ml, tempo_bomba_ms);

        gpio_set_level(dosador_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(tempo_bomba_ms));
        gpio_set_level(dosador_pin, 0);
    }

    // --- 4. Enche até o nível final (com timeout) ---
    ESP_LOGI(TAG, "Enchendo tanque ate nivel %d...", (int)nivel);
    const uint32_t TIMEOUT_MS = 60000; // 60 segundos
    const uint32_t TICK_TIMEOUT = pdMS_TO_TICKS(TIMEOUT_MS);
    TickType_t inicio = xTaskGetTickCount();

    while (!nivel_atingido(nivel))
    {
        if ((xTaskGetTickCount() - inicio) >= TICK_TIMEOUT)
        {
            ESP_LOGW(TAG, "TIMEOUT: Nivel nao atingido! Fechando valvula.");
            gpio_set_level(valvula_pin, 0);
            ESP_LOGI(TAG, "<<< encher abortado por timeout");
            return;
        }

        verificar_abort();
        if (s_abort)
        {
            ESP_LOGW(TAG, "ABORT durante encher! Fechando válvula.");
            gpio_set_level(valvula_pin, 0);
            ESP_LOGI(TAG, "<<< encher abortado");
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // --- 5. Fecha a válvula ---
    gpio_set_level(valvula_pin, 0);
    ESP_LOGI(TAG, "<<< encher concluído");
}

void bater(uint32_t tempo_sec)
{
    ESP_LOGI(TAG, ">>> bater(tempo=%" PRIu32 " s)", tempo_sec);

    uint32_t ciclos = tempo_sec / 5;
    uint32_t resto = tempo_sec % 5;

    for (uint32_t i = 0; i < ciclos; i++)
    {
        verificar_abort();
        if (s_abort)
        {
            ESP_LOGW(TAG, "ABORT durante bater! Desligando motor.");
            gpio_set_level(GPIO_SSR_MESTRE, 0);
            gpio_set_level(GPIO_MOTOR_HORARIO, 0);
            gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
            ESP_LOGI(TAG, "<<< bater abortado");
            return;
        }

        // Verifica se a tampa foi aberta
        if (gpio_get_level(GPIO_SENSOR_TAMPA) == 0)
        {
            ESP_LOGW(TAG, "Tampa aberta durante batimento! Desligando motor.");
            gpio_set_level(GPIO_SSR_MESTRE, 0);
            gpio_set_level(GPIO_MOTOR_HORARIO, 0);
            gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
            ESP_LOGI(TAG, "<<< bater abortado por segurança");
            return;
        }

        // --- Sentido Horário (2.5s) ---
        // 1. Desliga SSR (corta corrente)
        gpio_set_level(GPIO_SSR_MESTRE, 0);
        vTaskDelay(pdMS_TO_TICKS(500)); // Aguarda motor parar

        // 2. Configura relés para horário
        gpio_set_level(GPIO_MOTOR_HORARIO, 1);
        gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
        vTaskDelay(pdMS_TO_TICKS(100)); // Debounce dos contatos

        // 3. Liga SSR novamente
        gpio_set_level(GPIO_SSR_MESTRE, 1);
        vTaskDelay(pdMS_TO_TICKS(2400)); // 2.5s - 100ms já gastos

        // --- Sentido Anti-Horário (2.5s) ---
        // 1. Desliga SSR
        gpio_set_level(GPIO_SSR_MESTRE, 0);
        vTaskDelay(pdMS_TO_TICKS(500));

        // 2. Configura relés para anti-horário
        gpio_set_level(GPIO_MOTOR_HORARIO, 0);
        gpio_set_level(GPIO_MOTOR_ANTI_H, 1);
        vTaskDelay(pdMS_TO_TICKS(100));

        // 3. Liga SSR
        gpio_set_level(GPIO_SSR_MESTRE, 1);
        vTaskDelay(pdMS_TO_TICKS(2400));
    }

    // --- Trata o resto (se houver), com verificação de abort e tampa ---
    if (resto > 0)
    {
        // Sempre inicia no sentido horário (mais seguro para a máquina)
        gpio_set_level(GPIO_SSR_MESTRE, 0);
        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(GPIO_MOTOR_HORARIO, 1);
        gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
        vTaskDelay(pdMS_TO_TICKS(100));

        gpio_set_level(GPIO_SSR_MESTRE, 1);

        // Aciona o motor em steps de 100ms para permitir verificar abort e tampa
        uint32_t resto_ms = resto * 1000;
        uint32_t decorrido_ms = 0;
        while (decorrido_ms < resto_ms)
        {
            verificar_abort();
            if (s_abort)
            {
                ESP_LOGW(TAG, "ABORT durante bater (resto)! Desligando motor.");
                gpio_set_level(GPIO_SSR_MESTRE, 0);
                gpio_set_level(GPIO_MOTOR_HORARIO, 0);
                gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
                ESP_LOGI(TAG, "<<< bater abortado");
                return;
            }

            // Verifica se a tampa foi aberta
            if (gpio_get_level(GPIO_SENSOR_TAMPA) == 0)
            {
                ESP_LOGW(TAG, "Tampa aberta durante batimento (resto)! Desligando motor.");
                gpio_set_level(GPIO_SSR_MESTRE, 0);
                gpio_set_level(GPIO_MOTOR_HORARIO, 0);
                gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
                ESP_LOGI(TAG, "<<< bater abortado por segurança");
                return;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            decorrido_ms += 100;
        }
    }

    // --- Desliga tudo ao final ---
    gpio_set_level(GPIO_SSR_MESTRE, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    gpio_set_level(GPIO_MOTOR_HORARIO, 0);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);

    ESP_LOGI(TAG, "<<< bater concluído");
}

void centrifugar(uint32_t tempo_sec)
{
    ESP_LOGI(TAG, ">>> centrifugar(tempo=%" PRIu32 " s)", tempo_sec);

    /* Verifica se a tampa está fechada antes de começar */
    if (gpio_get_level(GPIO_SENSOR_TAMPA) == 0)
    {
        ESP_LOGW(TAG, "Tampa aberta! Não é possível centrifugar.");
        return;
    }

    // Sequência segura de partida: relé → delay → SSR (mesmo padrão de bater())
    // Garante que os contatos do relé estabilizem antes de energizar o motor via SSR
    gpio_set_level(GPIO_SSR_MESTRE, 0);
    gpio_set_level(GPIO_MOTOR_HORARIO, 1);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
    vTaskDelay(pdMS_TO_TICKS(100)); // Aguarda estabilização dos contatos
    gpio_set_level(GPIO_SSR_MESTRE, 1);

    // Monitora a tampa durante a centrifugação
    uint32_t tempo_decorrido_ms = 0;
    uint32_t tempo_total_ms = tempo_sec * 1000;
    while (tempo_decorrido_ms < tempo_total_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        tempo_decorrido_ms += 100;

        // Se a tampa abrir durante a centrifugação, para imediatamente
        if (gpio_get_level(GPIO_SENSOR_TAMPA) == 0)
        {
            ESP_LOGW(TAG, "Tampa aberta durante centrifugação! Parando motor.");
            gpio_set_level(GPIO_SSR_MESTRE, 0);
            gpio_set_level(GPIO_MOTOR_HORARIO, 0);
            gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
            ESP_LOGI(TAG, "<<< centrifugar abortado por segurança");
            return;
        }

        // Verifica abort solicitado pelo botão
        verificar_abort();
        if (s_abort)
        {
            ESP_LOGW(TAG, "ABORT durante centrifugação! Parando motor.");
            gpio_set_level(GPIO_SSR_MESTRE, 0);
            gpio_set_level(GPIO_MOTOR_HORARIO, 0);
            gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
            ESP_LOGI(TAG, "<<< centrifugar abortado");
            return;
        }
    }

    // SSR desliga primeiro (corta corrente do motor)
    gpio_set_level(GPIO_SSR_MESTRE, 0);

    // Pequena pausa para garantir que o SSR desligou
    vTaskDelay(pdMS_TO_TICKS(100));

    // Depois desliga os relés (sem corrente, sem arco elétrico)
    gpio_set_level(GPIO_MOTOR_HORARIO, 0);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);

    // Aguarda o cesto desacelerar
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "<<< centrifugar concluído");
}

void esvaziar(void)
{
    ESP_LOGI(TAG, ">>> esvaziar()");

    const uint32_t LIMIAR_VAZIO = LIMIAR_TANQUE_VAZIO_MV;
    const uint32_t TIMEOUT_MS = 5 * 60 * 1000; // 5 minutos
    const uint32_t TICK_TIMEOUT = pdMS_TO_TICKS(TIMEOUT_MS);
    TickType_t inicio = xTaskGetTickCount();

    ESP_LOGI(TAG, "Limiar de vazio: %u mV", LIMIAR_VAZIO);

    gpio_set_level(GPIO_BOMBA_DREENO, 1);

    while (1)
    {
        uint32_t tensao = ler_tensao_adc();

        // Verifica se já está vazio
        if (tensao < LIMIAR_VAZIO)
        {
            ESP_LOGI(TAG, "Tanque vazio detectado: %u mV", tensao);
            break;
        }

        // Verifica timeout de segurança
        if ((xTaskGetTickCount() - inicio) >= TICK_TIMEOUT)
        {
            ESP_LOGW(TAG, "Timeout de %u ms atingido (ultima leitura: %u mV)", TIMEOUT_MS, tensao);
            break;
        }

        // Verifica abort solicitado pelo botão
        verificar_abort();
        if (s_abort)
        {
            ESP_LOGW(TAG, "ABORT durante esvaziar!");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    gpio_set_level(GPIO_BOMBA_DREENO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "<<< esvaziar concluído");
}