idf_component_register(
    SRCS "app_passport.c"
         "app_state.c"
         "app_nvs.c"
         "app_ble.c"
         "app_voice.c"
         "screen_rain.c"
         "screen_menu.c"
         "screen_talking.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash json
)
