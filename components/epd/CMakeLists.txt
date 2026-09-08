idf_component_register(
    SRCS
        "src/epd_i80_bus.c"
        "src/epd_panel.c"
        "src/epd_display.c"
        "src/epd_panels.c"
        "src/epd_fb.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_lcd            # i80 bus + panel IO
        esp_driver_gpio    # bit-banged gate/latch lines
        esp_timer          # row pacing and statistics
        tps65185           # PMIC: rails, VCOM, thermistor
    PRIV_REQUIRES
        esp_mm             # heap_caps_* for DMA-capable row buffers
)
