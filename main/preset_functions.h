/*
 * SPDX-FileCopyrightText: 2024 Retrofit Brastemp Project
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Funções base para presets da máquina de lavar.
 * encher   - enche o tanque com água (quente ou fria), tipo/quantiade de produto químico, e nível de água
 * bater    - bate/agita por um determinado tempo
 * esvaziar - esvazia o tanque
 */

#ifndef PRESET_FUNCTIONS_H
#define PRESET_FUNCTIONS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ------------------------------------------------------------------ */
    /*  Tipos enumerados                                                   */
    /* ------------------------------------------------------------------ */

    /** Tipo de água: quente ou fria */
    typedef enum
    {
        AGUA_FRIA = 0,
        AGUA_QUENTE = 1,
    } agua_t;

    /** Nível de água: 1 (mínimo) a 4 (máximo) */
    typedef enum
    {
        NIVEL_1 = 1,
        NIVEL_2 = 2,
        NIVEL_3 = 3,
        NIVEL_4 = 4,
    } nivel_agua_t;

    /** Tipo de produto químico: 0 = nenhum, 1 a 4 = tipos */
    typedef enum
    {
        PRODUTO_NENHUM = 0,
        PRODUTO_1 = 1,
        PRODUTO_2 = 2,
        PRODUTO_3 = 3,
        PRODUTO_4 = 4,
    } produto_quimico_t;

    /* ------------------------------------------------------------------ */
    /*  Controle de abort                                                  */
    /* ------------------------------------------------------------------ */

    /**
     * @brief Obtém o estado da flag de abort.
     *
     * @return true se um abort foi solicitado.
     */
    bool obter_abort(void);

    /**
     * @brief Solicita o abort da operação atual.
     *
     * Define a flag de abort para true.
     * Chamado pelo botão Iniciar/Abortar quando um ciclo está rodando.
     */
    void solicitar_abort(void);

    /**
     * @brief Limpa a flag de abort.
     *
     * Deve ser chamada antes de iniciar um novo ciclo.
     */
    void limpar_abort(void);

/* ------------------------------------------------------------------ */
/*  Constantes públicas                                               */
/* ------------------------------------------------------------------ */

/** Limiar de tensão para considerar o tanque vazio (mV). */
#define LIMIAR_TANQUE_VAZIO_MV 25

    /* ------------------------------------------------------------------ */
    /*  Funções                                                            */
    /* ------------------------------------------------------------------ */

    /**
     * @brief Inicializa todos os GPIOs usados pelas funções de lavagem.
     *        Deve ser chamada uma vez durante o boot.
     */
    void configurar_gpios_preset(void);

    /**
     * @brief Lê o valor atual do sensor de pressão no GPIO 4 via ADC.
     *
     * @return Tensão em milivolts (0 a 3300).
     */
    uint32_t ler_pressao_adc_mv(void);

    /**
     * @brief Enche o tanque com os parâmetros especificados.
     *
     * @param agua             Água fria (AGUA_FRIA) ou quente (AGUA_QUENTE).
     * @param produto          Tipo de produto químico (PRODUTO_1 a PRODUTO_4).
     * @param quantidade_ml    Volume do produto químico em mililitros (ml).
     *                         Bomba peristáltica de 400 ml/min, tempo = ml * 150 ms.
     * @param nivel            Nível de água (NIVEL_1 a NIVEL_4).
     */
    void encher(agua_t agua, produto_quimico_t produto, uint16_t quantidade_ml, nivel_agua_t nivel);

    /**
     * @brief Bate/agita o tambor por um determinado tempo.
     *
     * @param tempo_sec  Tempo de agitação em segundos.
     */
    void bater(uint32_t tempo_sec);

    /**
     * @brief Centrifuga o tambor em alta rotação por um determinado tempo.
     *
     * @param tempo_sec  Tempo de centrifugação em segundos.
     */
    void centrifugar(uint32_t tempo_sec);

    /**
     * @brief Esvazia o tanque (aciona a bomba de drenagem).
     */
    void esvaziar(void);

#ifdef __cplusplus
}
#endif

#endif /* PRESET_FUNCTIONS_H */