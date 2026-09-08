idf_component_register(
    SRCS
        "epd_lvgl_main.c"
        "app_epd_lvgl.c"
    INCLUDE_DIRS "."
    REQUIRES epdinky_p4_board epd tps65185
)
