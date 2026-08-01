/*
 * SPDX-FileCopyrightText: 2024 Retrofit Brastemp Project
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Implementação das funções base para presets da máquina de lavar.
 */

#include "preset_functions.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "preset_functions";

/* ------------------------------------------------------------------ */
/*  Flag de iniciar compartilhada                                     */
/* ------------------------------------------------------------------ */

static volatile bool s_iniciar = false;

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
    ESP_LOGI(TAG, "ABORT solicitado");
    s_abort = true;
}

void limpar_abort(void)
{
    s_abort = false;
}

/* ------------------------------------------------------------------ */
/*  Flag de pausa compartilhada                                        */
/* ------------------------------------------------------------------ */

static volatile bool s_pausa = false;

bool obter_pausa(void)
{
    return s_pausa;
}

void pausar(void)
{
    ESP_LOGI(TAG, "PAUSA solicitada");
    s_pausa = true;
    led_pausa(true);
}

void continuar(void)
{
    ESP_LOGI(TAG, "CONTINUAR solicitado");
    s_pausa = false;
    led_pausa(false);
}

void limpar_pausa(void)
{
    s_pausa = false;
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

/* Entrada digital para teste em bancada: 1 clique = 50%, 2 cliques = 100% */
#define GPIO_TEST_NIVEL ((gpio_num_t)8) /* Sinal digital: teste de nivel */

/* LED dos botões (via relé 12V) */
#define GPIO_LED_INICIAR ((gpio_num_t)21)   /* LED do Botão Iniciar/Abortar — HIGH = máquina operando */
#define GPIO_LED_PAUSA_PLAY ((gpio_num_t)1) /* LED do Botão Pausar/Play         — HIGH = LED aceso */

/* Válvulas de água */
#define GPIO_VALVULA_AGUA_FRIA ((gpio_num_t)13)   /* Válvula Solenoide - Água Fria   */
#define GPIO_VALVULA_AGUA_QUENTE ((gpio_num_t)18) /* Válvula Solenoide - Água Quente */

/* Motor (SSR Mestre + relés de direção) */
#define GPIO_SSR_MESTRE ((gpio_num_t)15)    /* SSR Mestre (liga/desliga motor) */
#define GPIO_MOTOR_HORARIO ((gpio_num_t)14) /* Relé Horário                    */
#define GPIO_MOTOR_ANTI_H ((gpio_num_t)12)  /* Relé Anti-Horário               */

/* Bomba de drenagem */
#define GPIO_BOMBA_DREENO ((gpio_num_t)9) /* Bomba de Drenagem */

/* Bombas de produto químico */
#define GPIO_BOMBA_DETERGENTE ((gpio_num_t)16)    /* Bomba Detergente    (PRODUTO_1) - GPIO seguro */
#define GPIO_BOMBA_CLARIFICANTE ((gpio_num_t)10)  /* Bomba Clarificante  (PRODUTO_2) - GPIO seguro (GPIO 3 e strapping pin, evitado) */
#define GPIO_BOMBA_NEUTRALIZANTE ((gpio_num_t)39) /* Bomba Neutralizante (PRODUTO_3) */
#define GPIO_BOMBA_ESSENCIA ((gpio_num_t)7)       /* Bomba Essência      (PRODUTO_4) */

/* ------------------------------------------------------------------ */
/*  Constantes de calibração (ajustar manualmente)                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Histerese para leituras do sensor de pressão (mV).
 *        Evita oscilação quando a tensão está próxima ao limiar.
 */
#define HISTERESE_NIVEL_MV 50

/**
 * @brief Mapeamento de níveis de água para tensão do sensor.
 *        Sensor QDW90A: 0V = vazio, 3.3V = 0.5 bar (~5 metros de água).
 */
#define NIVEL_1_MV 825  // 25% do nível máximo
#define NIVEL_2_MV 1650 // 50% do nível máximo
#define NIVEL_3_MV 2475 // 75% do nível máximo
#define NIVEL_4_MV 3300 // 100% do nível máximo

/** @brief Tempo máximo entre cliques do teste de nível (ms) */
#define TESTE_CLIQUE_TIMEOUT_MS 800

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
        /* O sensor QDW90A possui saída push-pull; pull interno pode distorcer
         * a leitura. Se o seu sensor for open-drain, habilite o pull-down. */
        /* gpio_set_pull_mode(GPIO_SENSOR_PRESSAO, GPIO_PULLDOWN_ONLY); */

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
        esp_err_t ret_cali = adc_cali_raw_to_voltage(cali_handle, raw, &tensao_mv);
        if (ret_cali != ESP_OK)
        {
            ESP_LOGW(TAG, "Falha na conversao ADC calibrada (%s), usando raw", esp_err_to_name(ret_cali));
            tensao_mv = raw * 3300 / 4095;
        }
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
 * @brief Lê o estado do botão de teste de nível (GPIO8) e retorna qual
 *        condição foi solicitada através de cliques.
 *
 * Lógica:
 *   - 1 pulso (1 clique)   → retorna 50  (atingiu 50% do nível)
 *   - 2 pulsos (2 cliques) → retorna 100 (atingiu 100% do nível)
 *   - Nenhum pulso         → retorna 0   (sem sinal de teste)
 *
 * Deve ser chamada periodicamente (ex.: a cada 100 ms) para detectar
 * bordas de subida e timeout entre cliques.
 *
 * @return 0, 50 ou 100.
 */
static int detectar_teste_nivel(void)
{
    static uint8_t ultimo_level = 1;            /* Último nível lido (pull-up = 1) */
    static int contagem_cliques = 0;            /* Cliques detectados na janela atual */
    static TickType_t tick_primeiro_clique = 0; /* Tick do primeiro clique */
    static uint8_t aguardando_timeout = 0;      /* 1 = aguardando timeout entre cliques */
    static int resultado_pendente = 0;          /* Resultado a ser retornado na próxima chamada */

    /* Se há resultado pendente, retorna e limpa */
    if (resultado_pendente != 0)
    {
        int ret = resultado_pendente;
        resultado_pendente = 0;
        return ret;
    }

    uint8_t level_atual = gpio_get_level(GPIO_TEST_NIVEL);

    /* Detecta borda de descida: 1 → 0 (pressionou → GND) */
    if (ultimo_level == 1 && level_atual == 0)
    {
        contagem_cliques++;
        if (contagem_cliques == 1)
        {
            tick_primeiro_clique = xTaskGetTickCount();
        }
        aguardando_timeout = 1;
        ESP_LOGD(TAG, "Teste nivel: clique %d detectado", contagem_cliques);
    }

    /* Se está aguardando timeout, verifica se expirou */
    if (aguardando_timeout && contagem_cliques > 0)
    {
        TickType_t agora = xTaskGetTickCount();
        if ((agora - tick_primeiro_clique) >= pdMS_TO_TICKS(TESTE_CLIQUE_TIMEOUT_MS))
        {
            /* Timeout: processa a contagem */
            if (contagem_cliques >= 2)
            {
                ESP_LOGI(TAG, "Teste nivel: 2 cliques -> 100%%");
                resultado_pendente = 100;
            }
            else
            {
                ESP_LOGI(TAG, "Teste nivel: 1 clique -> 50%%");
                resultado_pendente = 50;
            }
            contagem_cliques = 0;
            aguardando_timeout = 0;
        }
    }

    ultimo_level = level_atual;
    return 0;
}

/**
 * @brief Retorna true se o sinal de teste indicar 50% do nível.
 *        Usado em bancada para simular 50% do nível sem sensor de pressão.
 */
static bool teste_nivel_50_atingido(void)
{
    return (detectar_teste_nivel() == 50);
}

/**
 * @brief Retorna true se o sinal de teste indicar 100% do nível.
 *        Usado em bancada para simular 100% do nível sem sensor de pressão.
 */
static bool teste_nivel_100_atingido(void)
{
    return (detectar_teste_nivel() == 100);
}

/**
 * @brief Retorna se o nível de água atual já atingiu o desejado.
 *        Leva em conta o sensor ADC OU as entradas digitais de teste (OR).
 *
 * @param nivel_desejado  Nível desejado (NIVEL_1 a NIVEL_4).
 * @return true  se o nível medido >= desejado OU sinal de teste ativo.
 * @return false se ainda está abaixo.
 */
static bool nivel_atingido(nivel_agua_t nivel_desejado)
{
    uint32_t tensao = ler_tensao_adc();
    uint32_t limiar_mv = obter_limiar_nivel(nivel_desejado);

    ESP_LOGD(TAG, "Sensor: %" PRIu32 " mV, limiar nivel %d: %" PRIu32 " mV", tensao, (int)nivel_desejado, limiar_mv);

    bool adc_ok = (tensao >= limiar_mv);
    bool teste_ok = teste_nivel_100_atingido();

    return (adc_ok || teste_ok);
}

/**
 * @brief Verifica se o tanque está vazio usando histerese.
 *
 * @param ultimo_estado_vazio  Ponteiro para manter o estado anterior.
 *                             Pode ser NULL (sem histerese).
 * @return true se o tanque está vazio.
 */
static bool tanque_vazio_com_histerese(bool *ultimo_estado_vazio)
{
    uint32_t tensao = ler_tensao_adc();
    bool vazio = (tensao < LIMIAR_TANQUE_VAZIO_MV);

    if (ultimo_estado_vazio != NULL)
    {
        if (!vazio && (tensao >= (LIMIAR_TANQUE_VAZIO_MV + HISTERESE_NIVEL_MV)))
        {
            *ultimo_estado_vazio = false;
        }
        else if (vazio)
        {
            *ultimo_estado_vazio = true;
        }
        return *ultimo_estado_vazio;
    }

    return vazio;
}

/**
 * @brief Para o motor de forma segura: SSR primeiro, depois relés.
 */
void parar_motor_seguro(void)
{
    gpio_set_level(GPIO_SSR_MESTRE, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_MOTOR_HORARIO, 0);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
}

/**
 * @brief Retorna true se desbalanceamento foi detectado.
 */
static bool desbalanceamento_detectado(void)
{
    return (gpio_get_level(GPIO_SENSOR_DESBALANCEAMENTO) == 0);
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

    /* Bombas de produto químico */
    const gpio_num_t bombas[] = {
        GPIO_BOMBA_DETERGENTE,
        GPIO_BOMBA_CLARIFICANTE,
        GPIO_BOMBA_NEUTRALIZANTE,
        GPIO_BOMBA_ESSENCIA,
    };
    for (int i = 0; i < (sizeof(bombas) / sizeof(bombas[0])); i++)
    {
        gpio_reset_pin(bombas[i]);
        gpio_set_direction(bombas[i], GPIO_MODE_OUTPUT);
        gpio_set_level(bombas[i], 0);
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

    /* LED do botão Iniciar/Abortar */
    gpio_reset_pin(GPIO_LED_INICIAR);
    gpio_set_direction(GPIO_LED_INICIAR, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_LED_INICIAR, 0);

    /* LED do botão Pausar/Play */
    gpio_reset_pin(GPIO_LED_PAUSA_PLAY);
    gpio_set_direction(GPIO_LED_PAUSA_PLAY, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_LED_PAUSA_PLAY, 0);

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

    /* --- ENTRADA DE TESTE EM BANCADA (GPIO8) --- */

    /* 1 clique = 50%, 2 cliques = 100% */
    gpio_reset_pin(GPIO_TEST_NIVEL);
    gpio_set_direction(GPIO_TEST_NIVEL, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_TEST_NIVEL, GPIO_PULLUP_ONLY);
}

void encher(agua_t agua, produto_quimico_t produto, uint16_t quantidade_ml, nivel_agua_t nivel)
{
    ESP_LOGI(TAG, ">>> encher(agua=%s, produto=%d, quantidade_ml=%u, nivel=%d)",
             agua == AGUA_QUENTE ? "QUENTE" : "FRIA",
             (int)produto, quantidade_ml, (int)nivel);

    /* --- 1. Abre a válvula de água --- */
    gpio_num_t valvula_pin = (agua == AGUA_QUENTE) ? GPIO_VALVULA_AGUA_QUENTE
                                                   : GPIO_VALVULA_AGUA_FRIA;
    gpio_set_level(valvula_pin, 1);

    /* --- 2. Aguarda atingir 50% do nível para dosagem --- */
    uint32_t limiar_alvo = obter_limiar_nivel(nivel);
    uint32_t limiar_metade = limiar_alvo / 2;

    ESP_LOGI(TAG, "Aguardando 50%% do nivel (%" PRIu32 " mV) ou 1 clique no GPIO8...", limiar_metade);

    bool pausado = false;
    while (!(ler_tensao_adc() >= limiar_metade || teste_nivel_50_atingido()))
    {

        bool pausa_atual = obter_pausa();

        /* Detecta mudança no status de pausa (a flag é alterada por outra task) */
        if (pausa_atual != pausado)
        {
            pausado = pausa_atual;

            if (pausado)
            {
                gpio_set_level(valvula_pin, 0);
                ESP_LOGI(TAG, "PAUSA: valvula desligada");
            }
            else
            {
                gpio_set_level(valvula_pin, 1);
                ESP_LOGI(TAG, "CONTINUAR: valvula religada");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* --- 3. Dosagem do produto químico --- */
    gpio_num_t bomba_pin = GPIO_NUM_NC;
    uint8_t produto_valido = 1;
    switch (produto)
    {
    case PRODUTO_NENHUM:
        ESP_LOGI(TAG, "Nenhum produto quimico selecionado");
        produto_valido = 0;
        break;
    case PRODUTO_1:
        bomba_pin = GPIO_BOMBA_DETERGENTE;
        break;
    case PRODUTO_2:
        bomba_pin = GPIO_BOMBA_CLARIFICANTE;
        break;
    case PRODUTO_3:
        bomba_pin = GPIO_BOMBA_NEUTRALIZANTE;
        break;
    case PRODUTO_4:
        bomba_pin = GPIO_BOMBA_ESSENCIA;
        break;
    default:
        ESP_LOGW(TAG, "Produto quimico invalido: %d", (int)produto);
        produto_valido = 0;
        break;
    }

    if (produto_valido && quantidade_ml > 0)
    {
        uint32_t tempo_bomba_ms = (uint32_t)quantidade_ml * 150u;
        ESP_LOGI(TAG, "Dosando %" PRIu32 " ml -> bomba ligada por %" PRIu32 " ms", (uint32_t)quantidade_ml, tempo_bomba_ms);

        gpio_set_level(bomba_pin, 1);

        uint32_t decorrido_ms = 0;
        while (decorrido_ms < tempo_bomba_ms)
        {

            bool pausa_atual = obter_pausa();

            /* Só liga/desliga a bomba quando o status de pausa MUDA */
            if (pausa_atual != pausado)
            {
                pausado = pausa_atual;

                if (pausado)
                {
                    gpio_set_level(bomba_pin, 0);
                    ESP_LOGI(TAG, "PAUSA: dosadora desligada");
                }
                else
                {
                    gpio_set_level(bomba_pin, 1);
                    ESP_LOGI(TAG, "CONTINUAR: dosadora religada");
                }
            }

            if (!pausado)
            {
                uint32_t passo = 100;
                if (passo > (tempo_bomba_ms - decorrido_ms))
                {
                    passo = tempo_bomba_ms - decorrido_ms;
                }
                decorrido_ms += passo;
                vTaskDelay(pdMS_TO_TICKS(passo));
            }
            else
            {
                /* Pausado: apenas aguarda e re-checa a flag */
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        gpio_set_level(bomba_pin, 0);
    }

    /* --- 4. Enche até o nível final --- */
    ESP_LOGI(TAG, "Enchendo tanque ate nivel %d...", (int)nivel);

    while (!nivel_atingido(nivel))
    {

        bool pausa_atual = obter_pausa();

        /* Detecta mudança no status de pausa (a flag é alterada por outra task) */
        if (pausa_atual != pausado)
        {
            pausado = pausa_atual;

            if (pausado)
            {
                gpio_set_level(valvula_pin, 0);
                ESP_LOGI(TAG, "PAUSA: valvula desligada");
            }
            else
            {
                gpio_set_level(valvula_pin, 1);
                ESP_LOGI(TAG, "CONTINUAR: valvula religada");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    gpio_set_level(valvula_pin, 0);
    ESP_LOGI(TAG, "<<< encher concluido");
}

#define BATIDA_TEMPO_GIRO_MS 2200
#define BATIDA_TEMPO_PARADA_MS 200
#define BATIDA_TEMPO_DEBOUNCE_RELE_MS 100
#define BATIDA_TEMPO_SENTIDO_MS (BATIDA_TEMPO_GIRO_MS + BATIDA_TEMPO_PARADA_MS + BATIDA_TEMPO_DEBOUNCE_RELE_MS)

void bater(uint32_t tempo_sec)
{
    ESP_LOGI(TAG, ">>> bater(tempo=%" PRIu32 " s)", tempo_sec);

    if (tempo_sec == 0)
    {
        ESP_LOGI(TAG, "<<< bater: tempo zero, nada a fazer");
        return;
    }

    uint32_t tempo_total_ms = tempo_sec * 1000;
    uint32_t decorrido_ms = 0;
    uint32_t ms_no_sentido = 0;

    bool sentido_horario = false;

    bool pausado = false;
    while (decorrido_ms < tempo_total_ms)
    {
        bool pausa_atual = obter_pausa();
        /* Detecta mudança no status de pausa (a flag é alterada por outra task) */
        if (pausa_atual != pausado)
        {
            pausado = pausa_atual;

            if (pausado)
            {
                gpio_set_level(GPIO_SSR_MESTRE, 0);
                gpio_set_level(GPIO_MOTOR_HORARIO, 0);
                gpio_set_level(GPIO_MOTOR_ANTI_H, 0);
                ESP_LOGI(TAG, "PAUSA: bater desligado");
            }
            else
            {
                sentido_horario = !sentido_horario;
                ESP_LOGI(TAG, "CONTINUAR: bater religado");
            }
        }

        if (pausado == false)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            decorrido_ms += 100;
            ms_no_sentido += 100;

            if (ms_no_sentido >= BATIDA_TEMPO_SENTIDO_MS && decorrido_ms < tempo_total_ms)
            {
                gpio_set_level(GPIO_SSR_MESTRE, 0);
                vTaskDelay(pdMS_TO_TICKS(BATIDA_TEMPO_PARADA_MS));

                sentido_horario = !sentido_horario;
                gpio_set_level(GPIO_MOTOR_HORARIO, sentido_horario ? 1 : 0);
                gpio_set_level(GPIO_MOTOR_ANTI_H, sentido_horario ? 0 : 1);
                vTaskDelay(pdMS_TO_TICKS(BATIDA_TEMPO_DEBOUNCE_RELE_MS));

                gpio_set_level(GPIO_SSR_MESTRE, 1);
                ms_no_sentido = 0;
                decorrido_ms += (BATIDA_TEMPO_PARADA_MS + BATIDA_TEMPO_DEBOUNCE_RELE_MS);
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    gpio_set_level(GPIO_SSR_MESTRE, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_MOTOR_HORARIO, 0);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);

    ESP_LOGI(TAG, "<<< bater concluido");
}

void centrifugar(uint32_t tempo_sec)
{
    ESP_LOGI(TAG, ">>> centrifugar(tempo=%" PRIu32 " s)", tempo_sec);

    if (ler_tensao_adc() > (LIMIAR_TANQUE_VAZIO_MV + HISTERESE_NIVEL_MV))
    {
        ESP_LOGW(TAG, "Tanque com agua! Esvazie antes de centrifugar.");
        pausar();
        return;
    }

    if (desbalanceamento_detectado())
    {
        ESP_LOGW(TAG, "Desbalanceamento detectado! Nao e possivel centrifugar.");
        pausar();
        return;
    }

    gpio_set_level(GPIO_SSR_MESTRE, 0);
    gpio_set_level(GPIO_MOTOR_HORARIO, 1);
    gpio_set_level(GPIO_MOTOR_ANTI_H, 0);

    for (int i = 0; i < 10; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        if (desbalanceamento_detectado())
        {
            ESP_LOGW(TAG, "Desbalanceamento durante partida da centrifugacao!");
            parar_motor_seguro();
            pausar();
            ESP_LOGI(TAG, "<<< centrifugar abortado por desbalanceamento");
            return;
        }
    }

    gpio_set_level(GPIO_SSR_MESTRE, 1);

    uint32_t tempo_decorrido_ms = 0;
    uint32_t tempo_total_ms = tempo_sec * 1000;
    while (tempo_decorrido_ms < tempo_total_ms)
    {

        vTaskDelay(pdMS_TO_TICKS(100));
        tempo_decorrido_ms += 100;

        if (desbalanceamento_detectado())
        {
            ESP_LOGW(TAG, "Desbalanceamento durante centrifugacao! Parando motor.");
            parar_motor_seguro();
            pausar();
            ESP_LOGI(TAG, "<<< centrifugar abortado por desbalanceamento");
            return;
        }
    }

    parar_motor_seguro();
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "<<< centrifugar concluido");
}

void esvaziar(void)
{
    ESP_LOGI(TAG, ">>> esvaziar()");

    bool estado_vazio = false;
    const uint32_t TIMEOUT_MS = 5 * 60 * 1000;
    const uint32_t TICK_TIMEOUT = pdMS_TO_TICKS(TIMEOUT_MS);
    TickType_t inicio = xTaskGetTickCount();

    ESP_LOGI(TAG, "Limiar de vazio: %" PRIu32 " mV", (uint32_t)LIMIAR_TANQUE_VAZIO_MV);

    gpio_set_level(GPIO_BOMBA_DREENO, 1);

    while (1)
    {

        uint32_t tensao = ler_tensao_adc();

        if (tanque_vazio_com_histerese(&estado_vazio))
        {
            ESP_LOGI(TAG, "Tanque vazio detectado: %" PRIu32 " mV", tensao);
            break;
        }

        if ((xTaskGetTickCount() - inicio) >= TICK_TIMEOUT)
        {
            ESP_LOGW(TAG, "Timeout de %" PRIu32 " ms atingido (ultima leitura: %" PRIu32 " mV)", TIMEOUT_MS, tensao);
            pausar();
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    gpio_set_level(GPIO_BOMBA_DREENO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "<<< esvaziar concluido");
}

void abrir_valvula_dreno(void)
{
    ESP_LOGI(TAG, ">>> abrir_valvula_dreno()");
    gpio_set_level(GPIO_BOMBA_DREENO, 1);
}

void fechar_valvula_dreno(void)
{
    ESP_LOGI(TAG, ">>> fechar_valvula_dreno()");
    gpio_set_level(GPIO_BOMBA_DREENO, 0);
}

void led_ciclo_rodando(bool ligar)
{
    gpio_set_level(GPIO_LED_INICIAR, ligar ? 1 : 0);
}

void led_pausa(bool ligar)
{
    gpio_set_level(GPIO_LED_PAUSA_PLAY, ligar ? 1 : 0);
}