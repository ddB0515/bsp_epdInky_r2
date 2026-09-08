# Arduino example for the epdInky ESP32-P4/C6

Uses the standard `setup()` / `loop()` entry points together with the `EpdInky`
C++ wrapper around the board support package, and demonstrates the sensors, the
micro-SD card, Wi-Fi and BLE.

## Build

The project pulls in `espressif/arduino-esp32` as a managed component and picks
up the BSP from `../../components`, so no manual setup is needed:

```sh
idf.py set-target esp32p4
idf.py build flash monitor
```

To let the button join a network, create `main/wifi_credentials.h`:

```sh
cp ../../main/wifi_credentials.h.example main/wifi_credentials.h
```

## Board-specific notes

Three settings here are not defaults and matter on this board:

- **`ARDUINO_USB_CDC_ON_BOOT=1` and `ARDUINO_USB_MODE=1`** (set in the top-level
  `CMakeLists.txt`). Arduino's `Serial` is UART0 by default, and on this board
  UART0 is GPIO37/GPIO38 — the TPS65185 `WAKEUP` and `INT` lines. These defines
  point `Serial` at the USB Serial/JTAG CDC device instead. They must be set
  build-wide, because arduino-esp32 only instantiates `HWCDCSerial` when it sees
  them.
- **`CONFIG_AUTOSTART_ARDUINO=y`**, so the core creates the Arduino task and
  calls `setup()`/`loop()`; the sketch needs no `app_main()`.
- **`CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y`**. arduino-esp32 3.3.11 predates
  the ESP-IDF 6.0 SPI HAL changes and trips a few default warnings that IDF
  normally promotes to errors. They are benign; remove this once arduino-esp32
  ships an IDF 6.0-clean release.

`CONFIG_FREERTOS_HZ=1000` is also required, but the board already sets it for
NimBLE.

## Using the library from the Arduino IDE

`components/epdinky_p4_board` doubles as an Arduino library (it ships
`library.properties`). Copy that folder into your Arduino `libraries/`
directory along with the five driver components, then `#include <bsp/EpdInky.h>`.

Note that Wi-Fi and BLE need the esp-hosted settings listed in the repository's
`sdkconfig.defaults`, which the Arduino IDE cannot change because it ships
prebuilt ESP-IDF libraries. Sensors, the button, the expander, the PMIC and the
SD card work regardless; for the radios, build through ESP-IDF as this example
does.
