idf_component_register(
    SRCS "epd_raw_main.c"
    INCLUDE_DIRS "."
    REQUIRES epdinky_p4_board epd tps65185
)
