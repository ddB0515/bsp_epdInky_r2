idf_component_register(
    SRCS "arduino_epd_raw.cpp"
    INCLUDE_DIRS "."
    REQUIRES epdinky_p4_board epd arduino-esp32
)
