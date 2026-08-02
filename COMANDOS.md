# 🚀 Comandos ESP-IDF — Retrofit Brastemp

> Arquivo de consulta rápida. Salve-o no projeto para não esquecer os comandos.

---

## 1. Ativar o ambiente (uma vez por terminal)

**PowerShell** (recomendado):

```powershell
& "C:\esp-idf\export.ps1"
```

> ⚠️ Sempre abra um **terminal novo** ao ativar o ambiente.
> NUNCA ative ambientes de versões diferentes no mesmo terminal.

---

## 2. Fluxo completo de trabalho

| Passo             | Comando                     | O que faz                                          |
| ----------------- | --------------------------- | -------------------------------------------------- |
| Definir chip alvo | `idf.py set-target esp32s3` | Define o chip (só na 1ª vez ou ao trocar de placa) |
| Configurar        | `idf.py menuconfig`         | Abre o editor de configurações (GPIOs, debounce)   |
| Compilar          | `idf.py build`              | Compila tudo                                       |
| Gravar            | `idf.py -p COM12 flash`     | Grava o firmware na porta COM12                    |
| Ver logs          | `idf.py -p COM12 monitor`   | Abre o terminal serial (sair com `Ctrl+]`)         |

---

## 3. Comandos do dia a dia (os mais usados)

```powershell
# Compilar + gravar + ver logs de uma vez:
idf.py -p COM12 flash monitor

# Só compilar:
idf.py build

# Só gravar:
idf.py -p COM12 flash

# Só ver logs:
idf.py -p COM12 monitor
```

---

## 4. Comandos utilitários

```powershell
# Ver a versão do ESP-IDF ativo:
idf.py --version

# Limpar o build (use se der erro estranho):
idf.py fullclean
```

---

## 5. Observações importantes

- **Porta COM12** — confirme no Gerenciador de Dispositivos que o ESP32-S3 está nela.
- **Sair do monitor** — pressione `Ctrl+]`.
- **Deixe o `C:\esp\v5.3.2\esp-idf` de lado** — o projeto usa a instalação `C:\esp-idf` (v6). Não misture versões.
- **`set-target` apaga configurações** — rode `menuconfig` de novo depois dele.
- **Projeto sem scripts `.bat`** — todos os comandos são digitados manualmente no terminal.

---

## 6. Fluxo de referência completa (PowerShell)

```powershell
& "C:\esp-idf\export.ps1"
idf.py set-target esp32s3      # apenas na primeira vez
idf.py menuconfig              # ajustar GPIOs/debounce quando necessário
idf.py -p COM12 flash monitor  # dia a dia: compila + grava + logs
```
