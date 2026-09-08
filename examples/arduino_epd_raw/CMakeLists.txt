cmake_minimum_required(VERSION 3.16)

# Reuse the BSP, the epd driver and the PMIC driver from the parent repository
# rather than duplicating them here.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# These must reach every component - including arduino-esp32 itself, which only
# instantiates HWCDCSerial when they are set - so they go on the build-wide
# property rather than on a single component.
#
# On this board UART0 is GPIO37/GPIO38, which are the TPS65185 WAKEUP and INT
# lines, so Arduino's default `Serial` (= UART0) must not be used. Pointing
# `Serial` at the USB Serial/JTAG CDC device matches the ESP-IDF console.
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_CDC_ON_BOOT=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_MODE=1" APPEND)

project(arduino_epd_raw)
