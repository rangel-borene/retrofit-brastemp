# AGENTS.md

## Purpose

This file helps AI coding agents understand the `blink` example and work productively with this ESP-IDF project.

## Project type

- ESP-IDF example project demonstrating LED blinking.
- Supports both regular GPIO LEDs and addressable LED strips via the `led_strip` component.
- Uses CMake with minimal build enabled (`idf_build_set_property(MINIMAL_BUILD ON)`).

## Key locations

- `main/blink_example_main.c` – application logic, LED configuration, and main loop.
- `main/CMakeLists.txt` – project entry for the `main` component.
- `main/idf_component.yml` – managed component dependency on `espressif/led_strip`.
- `sdkconfig.defaults.*` – target-specific default configurations.
- `managed_components/` – managed component checkout for `led_strip`.
- `build/` – generated build outputs; ignore for source changes.

## Build and flash

Recommended workflow:

1. Set the ESP target: `idf.py set-target <chip_name>`
2. Configure: `idf.py menuconfig`
3. Build and flash: `idf.py -p PORT flash monitor`

If asked to test or debug, use `idf.py monitor` after flash.

## Repository conventions

- Keep source code changes in `main/`.
- Use `sdkconfig` options for feature selection:
  - `CONFIG_BLINK_LED_GPIO` for GPIO LED mode.
  - `CONFIG_BLINK_LED_STRIP` for addressable LED strip.
- Do not modify generated `build/` outputs directly.

## Notes for font-related requests

- This repository does not contain any existing font files, font rendering code, or display subsystem support.
- If a task mentions `font`, first verify whether it belongs to another ESP-IDF example or requires adding a new display/font component.
- Avoid assuming font support exists in this blink example; focus on the existing LED blink behavior unless explicitly asked to expand the project scope.

## Useful references

- `README.md` — example usage, configuration, and troubleshooting.

## Behavior for AI agents

- Prefer minimal, example-focused changes.
- Preserve the example's intent: blinking an LED or LED strip.
- For feature additions, ensure they align with ESP-IDF component and CMake conventions.
