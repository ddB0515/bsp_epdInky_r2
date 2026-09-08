cmake_minimum_required(VERSION 3.16)

# Reuse the BSP and its device drivers from the parent repository rather than
# duplicating them here.
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

# FastEPD's header derives FASTEPD from Print when ARDUINO is defined, and not
# otherwise. That changes whether the class has a vtable, so the sketch and the
# FastEPD component must agree: compiling the sketch as Arduino while FastEPD
# built the plain version fails at link time with an undefined reference to
# `vtable for FASTEPD`. Setting this build-wide keeps both on the same
# definition.
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO" APPEND)

project(arduino_epd_fastepd)

# FastEPD is a managed component and its own CMakeLists does not know about
# arduino-esp32. Once ARDUINO is defined it includes Wire.h, which drags in the
# whole Arduino core and its IDF dependencies, so rather than chase individual
# headers give it the same include set arduino-esp32 itself compiles against.
#
# Done here rather than by editing the managed component, which would be
# overwritten on the next dependency update.
idf_component_get_property(fastepd_lib ddb0515__fastepd COMPONENT_LIB)
idf_component_get_property(arduino_lib espressif__arduino-esp32 COMPONENT_LIB)
target_link_libraries(${fastepd_lib} PRIVATE ${arduino_lib})
