idf_component_register(
    SRCS
        "main.c"
        "ha_persist.c"
        "ha_button.c"
        "ha_panel_jd9168.c"
        "ha_touch_gt911.c"
        "ha_lvgl.c"
        "ha_setup_screen.c"
        "ha_ws.c"
        "ha_dashboard.c"
        "portal/ha_portal.c"
        "portal/ha_dns.c"
    INCLUDE_DIRS "." "portal"
    REQUIRES
        epdinky_p4_board      # Wi-Fi over esp-hosted, I2C bus, RTC, board init
        esp_lcd               # esp_lcd_panel_* core API
        esp_lcd_jd9168        # the DSI panel driver
        sgm37604a             # backlight controller on the display module
        tca6408               # the display adapter board's own expander
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server       # the provisioning portal
        # espressif/{lvgl,esp_lcd_touch_gt911,cjson,esp_websocket_client} are
        # managed dependencies (see idf_component.yml) - the component
        # manager adds their headers automatically without needing to be
        # named here.
)
