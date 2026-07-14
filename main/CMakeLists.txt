idf_component_register(SRCS "retrofit_brastemp.c" "preset_functions.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES 
                        esp_driver_gpio
                        nvs_flash
                        esp_adc)
