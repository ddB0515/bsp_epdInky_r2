idf_component_register(
    SRCS
        "main.c"
        "trmnl_http.c"
        "trmnl_api.c"
        "trmnl_image.c"
        "trmnl_display.c"
        "qrcodegen.c"
        "trmnl_persist.c"
        "trmnl_refresh.c"
        "trmnl_cache.c"
        "trmnl_time.c"
        "trmnl_sleep.c"
        "trmnl_button.c"
        "trmnl_battery.c"
        "portal/trmnl_portal.c"
        "portal/trmnl_dns.c"
    INCLUDE_DIRS "." "portal"
    REQUIRES
        epdinky_p4_board      # Wi-Fi over esp-hosted, I2C, board init, SD, RTC
        stc3115               # the fuel gauge
        epd                   # framebuffer + parallel panel driver
        esp_rom               # tinfl_decompress: the boot ROM's inflate
        esp_http_client
        esp_http_server       # the provisioning portal
        esp-tls
        nvs_flash
        esp_wifi
        esp_netif             # esp_netif_sntp: the clock
        esp_hw_support        # esp_sleep: the wait between cycles
        fatfs                 # the SD-card image cache
)
