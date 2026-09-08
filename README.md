# epdInky ESP32-P4/C6 — BSP, components and examples

Board support package for the **epdInky rev.2** board (ESP32-P4 host +
ESP32-C6-MINI-1 radio), plus a set of standalone device-driver components and
a dozen example firmwares built on top of them — raw E-Ink and MIPI-DSI
display drivers, sensors/PMIC/RTC/GPIO-expander drivers, Wi-Fi/BLE/micro-SD
via the C6 radio, and full applications (Home Assistant dashboards, a TRMNL
port, camera streaming, LVGL).

This repo has three layers, each independently usable:

1. **[The BSP](#board-support-package-bsp)** (`components/epdinky_p4_board`) —
   ties the whole board together: I2C bus, every on-board device, Wi-Fi/BLE
   over the C6, micro-SD, an Arduino C++ wrapper.
2. **[Components](#components)** (`components/*`) — the individual device
   drivers the BSP is built from. Each one also stands alone, with its own
   README, and can be pulled into a different project without the rest of the
   BSP.
3. **[Examples](#examples)** (`examples/*`) — complete, buildable firmwares,
   each its own ESP-IDF project.

## Setting up the build environment

**This is the step that catches people out.** The stock `$IDF_PATH/export.sh`
does **not** work on this machine, because ESP-IDF was installed with EIM
(ESP-IDF Installation Manager), which uses a different directory layout.
Running it gives:

```
ERROR: ESP-IDF Python virtual environment
"/home/user/.espressif/python_env/idf6.0_py3.12_env/bin/python" not found.
```

Use the wrapper in this repository instead — note the **leading dot**, it must
be sourced rather than executed:

```sh
cd ~/epdInky_bsp
. ./export-idf.sh
```

You should see:

```
ESP-IDF ready: ESP-IDF v6.0.2
IDF_PATH: /home/user/.espressif/v6.0.2/esp-idf
```

This has to be done **once per terminal**. Opening a new terminal means doing
it again.

## Build and flash

```sh
. ./export-idf.sh              # once per terminal
idf.py set-target esp32p4      # first time only, or after fullclean
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Exit the monitor with `Ctrl-]`.

### One-liner

```sh
cd ~/epdInky_bsp && . ./export-idf.sh && idf.py build && idf.py -p /dev/ttyACM0 flash monitor
```

## Board support package (BSP)

**[`components/epdinky_p4_board`](components/epdinky_p4_board/README.md)** —
the C API (plus a C++ `EpdInky` wrapper for Arduino sketches) that brings the
whole board up: I2C bus, every on-board device, Wi-Fi/BLE over the ESP32-C6
radio, and micro-SD.

Every peripheral can be brought up independently, or all at once:

```c
#include "bsp/epdinky_p4_board.h"

bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
cfg.enable.use_stc3115 = false;              // skip the fuel gauge, e.g.

bsp_epdinky_handles_t board;
ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));
```

| On-board device | Bus / address | BSP driver |
|---|---|---|
| TPS65185 | I2C `0x68` | E-Ink PMIC (VCOM + rails) — [`components/tps65185`](components/tps65185/README.md) |
| TCA6408A | I2C `0x21` | GPIO expander (SD power gate, sensor IRQ fan-in) — [`components/tca6408`](components/tca6408/README.md) |
| RV-3028-C7 | I2C `0x52` | RTC, optional on the schematic — [`components/rv3028`](components/rv3028/README.md) |
| KXTJ3-1057 | I2C `0x0F`/`0x0E` | Accelerometer, optional — [`components/kxtj3_1057`](components/kxtj3_1057/README.md) |
| STC3115 | I2C `0x70` | Fuel gauge — [`components/stc3115`](components/stc3115/README.md) |
| ESP32-C6-MINI-1 | SDIO slot 1 | Wi-Fi/BLE via [esp-hosted](https://components.espressif.com/components/espressif/esp_hosted) |
| micro-SD | SDMMC slot 0, 4-bit | Power-gated through the TCA6408 |

The BSP's own README has the full pin map, the SDIO/SD controller-sharing
workaround, why sensor interrupts never reach the CPU, why UART0 must stay
off this board, and every other hardware gotcha it encodes for you — read it
before wiring a new peripheral onto the shared I2C bus or SDIO controller:
**[components/epdinky_p4_board/README.md](components/epdinky_p4_board/README.md)**.

Two display panels are supported on top of the BSP, each its own component
(see [Components](#components) below): the board's **raw E-Ink** panels
(a 12-model catalogue, e.g. the 10.3" ED103TC2) via `epd` + `tps65185`, and
the 3.2" **MIPI-DSI TFT** adapter via `esp_lcd_jd9168` + `sgm37604a` (+ the
registry's `esp_lcd_touch_gt911` for its GT967 touch controller).

---

## Components

Each driver in `components/` is independently documented and independently
usable — pull just the one you need into another project. `epdinky_p4_board`
(the BSP, above) is built on top of all of them; nothing in this list depends
on the BSP itself.

| Component | Drives | |
|---|---|---|
| [`epd`](components/epd/README.md) | Raw E-Ink panels (bare glass + source/gate driver ICs, no TCON) over the ESP32 LCD i80 peripheral — INIT/GC16/DU waveforms, a 12-panel catalogue, 4bpp framebuffer | requires `tps65185` |
| [`esp_lcd_jd9168`](components/esp_lcd_jd9168/README.md) | JD9168 MIPI-DSI panel driver (D320C2403V-MIPI, 3.2" 1024×768 IPS), on `esp_lcd`'s panel interface | |
| [`tps65185`](components/tps65185/README.md) | TPS65185/86 E-Ink PMIC — VCOM, VDDH/VPOS/VNEG/VEE rails, thermistor | hard dependency of `epd` |
| [`sgm37604a`](components/sgm37604a/README.md) | SGM37604A I2C backlight/LED driver (the MIPI-DSI panel's backlight) | |
| [`tca6408`](components/tca6408/README.md) | TCA6408 8-bit I2C GPIO expander — two independent instances on this board, at different addresses, for unrelated purposes (see its README) | |
| [`kxtj3_1057`](components/kxtj3_1057/README.md) | Kionix KXTJ3-1057 3-axis I2C accelerometer | |
| [`rv3028`](components/rv3028/README.md) | Micro Crystal RV-3028-C7 battery-backed I2C RTC | |
| [`stc3115`](components/stc3115/README.md) | STMicroelectronics STC3115 I2C battery gas gauge | |
| [`epdinky_p4_board`](components/epdinky_p4_board/README.md) | The BSP itself — see [above](#board-support-package-bsp) | ties all of the above together |

Each README documents that component's real API, register map and known
bring-up gotchas — pulled from the source and, wherever one exists, from a
real call site in this repo's own examples, not from general datasheet
knowledge.

---

## Examples

Each example is a **separate ESP-IDF project** — `cd` into it before running
`idf.py`, and treat its own README as authoritative for anything beyond the
generic build/flash steps below:

```sh
cd examples/<name>
. ../../export-idf.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

The first build of each example downloads its own managed components and
takes several minutes. A few need one extra step before that — noted in the
table.

**Raw E-Ink (`epd` component, no vendor library):**

| Example | What it does | Extra setup |
|---|---|---|
| [`idf_epd_raw`](examples/idf_epd_raw/README.md) | Minimal raw E-Ink demo: INIT/GC16/DU waveforms, timing measurements | |
| [`idf_epd_lvgl`](examples/idf_epd_lvgl/README.md) | LVGL driving a 1872×1404 ED103TC2 through `epd`, with a refresh policy that batches LVGL's frequent small redraws instead of paying a full E-Ink refresh per one | |
| [`arduino_epd_raw`](examples/arduino_epd_raw/README.md) | Same idea as `idf_epd_raw`, as an Arduino `setup()`/`loop()` sketch | |
| [`idf_epd_ha_firmware`](examples/idf_epd_ha_firmware/README.md) | Full Home Assistant dashboard over MQTT: Wi-Fi captive-portal provisioning, battery gauge, deep-sleep between refreshes | captive portal on first boot — no `wifi_credentials.h` needed |
| [`trmnl-firmware`](examples/trmnl-firmware/README.md) | Native ESP-IDF port of [usetrmnl/trmnl-firmware](https://github.com/usetrmnl/trmnl-firmware) — no Arduino, no FastEPD/bb_epaper, panel driven by `epd` | captive portal on first boot |

**Raw E-Ink via the third-party FastEPD library** (comparison/reference —
see each README for why this repo also has its own `epd` driver instead):

| Example | What it does | Extra setup |
|---|---|---|
| [`idf_epd_fastepd_basic`](examples/idf_epd_fastepd_basic/README.md) | Same board, driven by [FastEPD](https://github.com/bitbank2/FastEPD) instead of `epd` | FastEPD is pulled from the component registry's **staging** channel — see the example's README |
| [`arduino_epd_fastepd`](examples/arduino_epd_fastepd/README.md) | FastEPD as an Arduino sketch | same staging-registry note |

**MIPI-DSI TFT panel (`esp_lcd_jd9168` + touch):**

| Example | What it does | Extra setup |
|---|---|---|
| [`idf_dsi_camera_preview`](examples/idf_dsi_camera_preview/README.md) | Live SC2336 camera preview on the DSI panel, LVGL UI, capacitive touch — also the reference bring-up for the panel/backlight/touch sequencing | |
| [`idf_ha_firmware`](examples/idf_ha_firmware/README.md) | Always-on, interactive Home Assistant dashboard on the TFT+touch panel, native WebSocket API, vanilla LVGL 9.5 (mains-powered) | edit the dashboard's entity list before flashing — see its README |

**Camera (MIPI-CSI, no display):**

| Example | What it does | Extra setup |
|---|---|---|
| [`idf_camera_webserver`](examples/idf_camera_webserver/README.md) | Streams the SC2336 camera over Wi-Fi: MJPEG/HTTP preview plus an H.264 RTSP stream | `cp main/wifi_credentials.h.example main/wifi_credentials.h` and edit it |

**Board bring-up:**

| Example | What it does | Extra setup |
|---|---|---|
| [`arduino_epdinky`](examples/arduino_epdinky/README.md) | Arduino `setup()`/`loop()` sketch exercising the BSP's `EpdInky` C++ wrapper (sensors, Wi-Fi, BLE) | |
| `main/` (this project, not under `examples/`) | The BSP's own C bring-up demo — both `bsp_epdinky_init_with_config()` and the individual `bsp_<device>_init()` calls | `cp main/wifi_credentials.h.example main/wifi_credentials.h` (optional — builds without it) |

## Troubleshooting

**`idf.py: command not found`** — the environment is not active in this
terminal. Run `. ./export-idf.sh` (with the leading dot).

**`ERROR: ESP-IDF Python virtual environment ... not found`** — you ran
`export.sh` instead of `. ./export-idf.sh`. See above.

**`Could not open /dev/ttyACM0, the port is busy`** — a serial monitor is
already attached. Close it, or find the holder with:

```sh
for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q ttyACM && echo "held by PID $(basename $p)"; done
```

**`attempt to rename spec 'link' to already defined spec 'picolibc_link'`** —
a stale `build/toolchain/cflags` has a duplicated `-specs=` entry, which happens
when `idf.py reconfigure` runs after new managed components are added:

```sh
idf.py fullclean && idf.py build
```

**Changes to `sdkconfig.defaults` seem to be ignored** — `sdkconfig` is
generated and takes precedence once it exists. Delete it and rebuild:

```sh
rm sdkconfig && idf.py build
```

**VS Code ESP-IDF extension fails** — `.vscode/settings.json` must point at the
real install. The correct values for this machine are:

```json
"idf.espIdfPath": "/home/user/.espressif/v6.0.2/esp-idf",
"idf.toolsPath": "/home/user/.espressif/tools",
"idf.pythonInstallPath": "/home/user/.espressif/tools/python/v6.0.2/venv/bin/python"
```

## Repository layout

```
components/
├── epdinky_p4_board/   the BSP: C API + EpdInky C++ wrapper (I2C, Wi-Fi/BLE, SD, every device below)
├── epd/                raw E-Ink panel driver (INIT/GC16/DU waveforms, panel catalogue, framebuffer)
├── esp_lcd_jd9168/     JD9168 MIPI-DSI panel driver (D320C2403V-MIPI, 3.2" 1024x768)
├── tps65185/           E-Ink PMIC driver (VCOM, VDDH/VPOS/VNEG/VEE rails) — epd's hard dependency
├── sgm37604a/          I2C backlight/LED driver (MIPI-DSI panel's backlight)
├── tca6408/            8-bit I2C GPIO expander driver
├── kxtj3_1057/         3-axis I2C accelerometer driver
├── rv3028/             battery-backed I2C RTC driver
└── stc3115/            I2C battery gas gauge driver

examples/
├── idf_epd_raw/            minimal raw E-Ink demo (epd component)
├── idf_epd_lvgl/           LVGL on a raw E-Ink panel (epd component)
├── arduino_epd_raw/        same as idf_epd_raw, Arduino sketch
├── idf_epd_ha_firmware/    Home Assistant e-paper dashboard over MQTT (battery, deep-sleep)
├── trmnl-firmware/         native ESP-IDF port of usetrmnl/trmnl-firmware
├── idf_epd_fastepd_basic/  raw E-Ink panel driven by the third-party FastEPD library instead
├── arduino_epd_fastepd/    same, Arduino sketch
├── idf_dsi_camera_preview/ SC2336 camera preview on the MIPI-DSI TFT panel, LVGL + touch
├── idf_ha_firmware/        Home Assistant dashboard on the MIPI-DSI TFT+touch panel (mains-powered)
├── idf_camera_webserver/   MIPI-CSI camera streaming server (MJPEG + RTSP)
└── arduino_epdinky/        Arduino setup()/loop() example using the EpdInky C++ wrapper

main/                    this project's own board bring-up demo (not an examples/ project)
```

See [components/epdinky_p4_board/README.md](components/epdinky_p4_board/README.md)
for the pin map, the full API and every board-specific gotcha, and each
component's/example's own README (linked above) for everything specific to
it.
