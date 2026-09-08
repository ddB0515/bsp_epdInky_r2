idf_component_register(
    SRCS
        "main.c"
        "ha_http.c"
        "ha_mqtt.c"
        "ha_image.c"
        "ha_display.c"
        "ha_persist.c"
        "ha_time.c"
        "ha_sleep.c"
        "ha_button.c"
        "ha_battery.c"
        "qrcodegen.c"
        "portal/ha_portal.c"
        "portal/ha_dns.c"
    INCLUDE_DIRS "." "portal"
    REQUIRES
        epdinky_p4_board      # Wi-Fi over esp-hosted, I2C, board init, RTC
        stc3115               # the fuel gauge
        epd                   # framebuffer + parallel panel driver
        esp_rom               # tinfl_decompress: the boot ROM's inflate
        esp_http_client
        esp_http_server       # the provisioning portal
        esp-tls
        mbedtls               # esp_crt_bundle_attach for HTTPS verification
        nvs_flash
        esp_wifi
        esp_netif             # esp_netif_sntp: the clock
        esp_hw_support        # esp_sleep: the wait between cycles
        # espressif/mqtt and espressif/cjson are managed dependencies (see
        # idf_component.yml) - the component manager adds their headers
        # automatically without needing to be named here, the same way the
        # trmnl-firmware example's cjson dependency works.
)
