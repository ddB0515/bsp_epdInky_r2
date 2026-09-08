idf_component_register(
    SRCS "arduino_epd_fastepd.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32 ddb0515__fastepd
)
