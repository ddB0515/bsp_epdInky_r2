idf_component_register(
    SRCS "epdinky_p4_board.c" "bsp_wifi.c" "bsp_ble.c" "EpdInky.cpp"
    INCLUDE_DIRS "include" "include/bsp"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_sdmmc
        sdmmc
        fatfs
        vfs
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        bt
        tps65185
        stc3115
        rv3028
        kxtj3_1057
        tca6408
    PRIV_REQUIRES esp_psram spi_flash
)
