| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- |

# Retrofit Brastemp

Projeto para controle de relé via botão com debounce, utilizando ESP-IDF.

## Funcionalidades

- Controle de um relé (para substituir/replicar funcionalidades de placas originais Brastemp)
- Botão com debounce por software para acionar o relé
- Pull-up interno ativado no GPIO do botão (dispensa resistor externo)
- Logs via UART para monitoramento

## Como Usar

### Hardware Necessário

- Placa de desenvolvimento ESP32/ESP32-S3/outra compatível
- Um relé (módulo relé) conectado ao GPIO configurado
- Um botão (push-button) conectado ao GPIO configurado (com GND)
- Fonte de alimentação e cabo USB para programação

### Configuração do Projeto

Execute `idf.py menuconfig` e vá em `Example Configuration`:

- **Relay GPIO number** — GPIO conectado ao sinal de controle do relé (padrão: GPIO4)
- **Button GPIO number** — GPIO conectado ao botão (padrão: GPIO0, com pull-up interno)
- **Debounce time in ms** — Tempo de debounce do botão (padrão: 20 ms)

### Build e Flash

```bash
idf.py set-target esp32s3   # ou outro target compatível
idf.py menuconfig            # ajustar GPIOs conforme necessário
idf.py -p PORT flash monitor # compilar, gravar e monitorar
```

Para sair do monitor serial, pressione `Ctrl+]`.

### Exemplo de Saída

```
I (315) retrofit_brastemp: Retrofit Brastemp - Relay Control
I (315) retrofit_brastemp: Relay GPIO: 4
I (315) retrofit_brastemp: Button GPIO: 0
I (315) retrofit_brastemp: Debounce time: 20 ms
I (1325) retrofit_brastemp: Relay ON
I (2325) retrofit_brastemp: Relay OFF
```

## Troubleshooting

- Verifique se os GPIOs configurados estão corretos
- Certifique-se de que o botão está conectado entre o GPIO e GND
- Para relés que exigem mais corrente, use um transistor driver ou módulo relé adequado

## Licença

Este projeto é baseado no exemplo `blink` do ESP-IDF (licença CC0-1.0).
