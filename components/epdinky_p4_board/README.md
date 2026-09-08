# BSP: epdInky ESP32-P4/C6 rev.2

Board Support Package for the epdInky e-Paper board (ESP32-P4 host + ESP32-C6-MINI-1 radio).

Every pin and address below was verified against the KiCad netlist exported from
`epdInkyP4C6.kicad_sch` (rev.2) — not read off the schematic PDF.

## Design

Each peripheral can be initialised **independently**. There is no hidden ordering
requirement except the two real hardware dependencies noted below.

```c
/* Bring up only what you need */
tca6408_handle_t expander;
ESP_ERROR_CHECK(bsp_tca6408_init(NULL, &expander));   /* NULL = board defaults */

/* ...or configure a set of them in one call */
bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
cfg.enable.use_stc3115 = false;          /* skip the fuel gauge */

bsp_epdinky_handles_t board;
ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));
```

Every `bsp_<device>_init()` brings the shared I2C bus up on demand, probes the
device first, and returns `ESP_ERR_NOT_FOUND` if it does not acknowledge.

For whole-board init, whether that is fatal comes from the schematic rather than
from caller configuration. The KXTJ3 accelerometer and RV-3028 RTC are marked
OPTIONAL, so if they do not respond they are skipped with a warning. Every other
device is required — a missing TCA6408 or TPS65185 fails init rather than
handing back a NULL handle that only bites later. Set
`cfg.allow_missing_required_devices` to downgrade that to a warning when
bringing up a partially populated board.

## Peripherals

| Device | Bus / address | Notes |
| --- | --- | --- |
| TPS651851 | I2C `0x68` | e-Ink PMIC. Rails stay **off** unless `opts.tps65185_power_up_rails` is set |
| TCA6408A | I2C `0x21` | GPIO expander, `ADDR` tied high |
| RV-3028-C7 | I2C `0x52` | RTC (marked optional on the schematic) |
| KXTJ3-1057 | I2C `0x0F` or `0x0E` | Accelerometer, address set by jumper JP2. Both are probed |
| STC3115 | I2C `0x70` | Fuel gauge |
| micro-SD | SDMMC slot 0, 4-bit | Power gated through the expander |
| Wi-Fi/BT | SDIO slot 1 | ESP32-C6 via esp-hosted |

## Pin map

**I2C** — `SDA` GPIO28, `SCL` GPIO29 @400 kHz.
External 2K2 pull-ups (R34/R38) are fitted, so the internal pull-ups are left **disabled**.

**EPD parallel bus** — `D0..D15` on GPIO2..GPIO17 (contiguous).

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| `SPV` | 45 | | `XCL` | 50 |
| `XSTL` (SPH) | 46 | | `CKV` | 51 |
| `XOE` | 47 | | `MODE` | 52 |
| `XLE` | 48 | | | |

`BORDER` is strapped through jumper JP1 — it is not software controllable.

**TPS65185** — `PWRUP` 26, `PWR_GOOD` 27 (in), `WAKEUP` 37, `INT` 38 (in, active low), `VCOM_CTRL` 49.

**micro-SD (slot 0)** — `CLK` 43, `CMD` 44, `D0` 39, `D1` 40, `D2` 41, `D3` 42.

**ESP32-C6 SDIO (slot 1)** — `CLK` 18, `CMD` 19, `D0` 23, `D1` 22, `D2` 21, `D3` 20, `EN` 54, `IO2` 53.

**User I/O** — SW4 button on GPIO35 (active low). SW3 drives `CHIP_PU` and is invisible to software.

**TCA6408 port bits** (expander pin indices, not GPIOs):

| Bit | Function | Direction |
| --- | --- | --- |
| P0 | KXTJ3 interrupt | input |
| P1 | RV3028 interrupt | input |
| P2–P5 | header J3 `EXP_IO` | input (safe default) |
| P6 | test point TP9 | input |
| P7 | SD card power gate | output, **active low** |

## Hardware gotchas

These are the non-obvious things the BSP encodes for you.

**SD card power runs through the expander.** `SD1-EN` (expander P7) drives the gate
of Q4, an AO3407 P-channel MOSFET feeding card VDD. Gate **low powers the card on**.
At reset the expander pins are inputs, and R54/R55 form a 10K/10K divider that leaves
the gate around 1.65 V — a partially-on state. `bsp_tca6408_init()` therefore writes
the output latch *before* switching P7 to an output, so the card never glitches on.
You must initialise the expander before using the SD slot.

> Note: software cannot guarantee the card is off between power-on and expander
> init. With the gate at ~1.65 V, Q4 sees V_GS ≈ −1.65 V, which is past the
> AO3407 threshold, so the card may be weakly powered during that window.
> Making reset-state OFF deterministic would need a board change (drop R54 so
> R55 holds the gate at 3V3).

**The SD card and Wi-Fi share one SDMMC controller.** The ESP32-P4 has a single
SDMMC controller with two slots: the card is on slot 0, the ESP32-C6 on slot 1.
`sdmmc_host_init()` is *not* reference counted in ESP-IDF — it unconditionally
creates a controller and returns `ESP_ERR_NOT_FOUND` if one already exists. Since
esp-hosted claims the controller first, a plain `esp_vfs_fat_sdmmc_mount()` fails
with `host init failed (0x105)`. The BSP installs its own `host.init` shim that
treats "already created" as success and attaches slot 0 to the existing
controller. Teardown needs no shim because `SDMMC_HOST_DEFAULT()` uses
`sdmmc_host_deinit_slot()`, which drops only our slot.

Verified working together: card at 20 MHz on slot 0 and the C6 at 40 MHz on
slot 1, with the card still readable while the Wi-Fi transport is active.

**Sensor interrupts do not reach the CPU.** The KXTJ3 and RV3028 interrupt pins land
on expander P0/P1. The only interrupt line to the P4 is the expander's aggregate
`INT` on GPIO34 (open-drain, active low). Polling works without the expander;
interrupt-driven use does not.

**Do not drive GPIO54 low.** `C6_CHIP_PU` has a 10K pull-up (R43), so the C6 boots
enabled. esp-hosted owns this pin and issues its own HIGH→LOW→HIGH reset pulse
(`CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`, which is correct for an EN pin).
Driving it low from application code holds the radio in reset.

**Keep the UART console off.** The ESP32-P4's default UART0 console pins are
GPIO37/GPIO38 — exactly the TPS65185 `WAKEUP` and `INT` lines on this board. With
the UART console enabled, every log line toggles the PMIC wake input. The project
therefore selects `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and leaves UART0 unused.

**Signals with no path to software:** SD card-detect (`SD1-CD`), STC3115 `ALM`,
RV3028 `CLKOUT` and `EVI`. Poll these devices instead of waiting on an interrupt.

**PMIC rails default to off.** `bsp_tps65185_init()` configures VCOM but does not
energise VPOS/VNEG/VGH/VGL unless you ask, so the panel is never driven
accidentally before a waveform driver is ready.

## Wi-Fi

The P4 has no radio. Wi-Fi runs on the ESP32-C6 over SDIO slot 1, with
[esp-hosted](https://components.espressif.com/components/espressif/esp_hosted)
carrying the normal `esp_wifi` API across the link. The C6 must be flashed with
matching esp-hosted **slave** firmware.

```c
ESP_ERROR_CHECK(bsp_wifi_init());     /* resets the C6, brings up SDIO + netif */
bsp_wifi_scan_print();                /* print nearby APs */

bsp_wifi_ap_t aps[16];
size_t found;
bsp_wifi_scan(aps, 16, &found);       /* or get them programmatically */

bsp_wifi_connect("my-ssid", "my-password", 20000);
if (bsp_wifi_is_connected()) { /* ... */ }
```

`bsp_wifi_init()` is idempotent and is called automatically by `bsp_wifi_scan()`
and `bsp_wifi_connect()`. Starting the radio does **not** join a network — call
`bsp_wifi_connect()` explicitly. Adding Wi-Fi grows the binary by roughly 700 KB,
so check that your app partition is large enough.

The demo app keeps its credentials in `main/wifi_credentials.h`, which is
gitignored; copy `main/wifi_credentials.h.example` to create it. Pressing the
user button (SW4) joins the network, and pressing again leaves it.

## SD card

```c
tca6408_handle_t expander;
bsp_tca6408_init(NULL, &expander);          /* required: it gates card power */
bsp_sdcard_mount(NULL, expander);           /* powers up + mounts /sdcard    */

FILE *f = fopen("/sdcard/hello.txt", "w");
/* ... */
bsp_sdcard_unmount(expander);               /* unmounts + powers down        */
```

Filenames follow FAT 8.3 unless you enable long-filename support
(`CONFIG_FATFS_LFN_HEAP`). See the SDMMC controller-sharing note below.

## Bluetooth LE

BLE runs over the same esp-hosted SDIO link as Wi-Fi: the NimBLE **host** runs
on the P4 and the controller lives on the C6, which reports *BLE only* — classic
Bluetooth is not available.

```c
ESP_ERROR_CHECK(bsp_ble_init());     /* enables the remote controller + NimBLE */
bsp_ble_scan_print(5000);            /* print nearby devices for 5 s */

bsp_ble_device_t devs[16];
size_t found;
bsp_ble_scan(devs, 16, &found, 5000);
```

Duplicate advertisements are merged, so each device appears once with its latest
RSSI and, where advertised, its name.

Two non-obvious requirements, both handled by the BSP/defaults:

- **The co-processor's BT controller is disabled at boot.** Since esp-hosted
  v2.5.2 it must be switched on explicitly (`esp_hosted_bt_controller_init()` and
  `_enable()`) *before* starting NimBLE — the slave advertises BLE in its
  capabilities either way, so skipping this looks like a working setup but makes
  every HCI command time out with `BLE_HS_ETIMEOUT_HCI`. `bsp_ble_init()` does it.
- **`CONFIG_FREERTOS_HZ` must be 1000.** At the IDF default of 100 the 10 ms tick
  granularity breaks NimBLE's HCI acknowledgement handling, with the same
  timeout symptom.

Also note `CONFIG_BT_NIMBLE_TRANSPORT_UART` must be `n`: with no local controller
NimBLE defaults to a UART HCI transport, and while that is set the esp-hosted
VHCI option is not even selectable.

## Arduino

The BSP ships a C++ wrapper (`bsp/EpdInky.h`) and doubles as an Arduino library.
Sketches use the usual `setup()`/`loop()` entry points:

```cpp
#include <Arduino.h>
#include "bsp/EpdInky.h"

void setup() {
    Serial.begin(115200);
    EpdInkyOptions options;
    options.sdCard = true;
    EpdInky.begin(options);

    EpdInky.wifiBegin();
    EpdInky.wifiScanPrint();
    EpdInky.bleBegin();
    EpdInky.bleScanPrint(5000);
}

void loop() {
    EpdInkyAccel accel;
    if (EpdInky.readAccel(accel)) {
        Serial.printf("X=%.0f Y=%.0f Z=%.0f mg\n", accel.x, accel.y, accel.z);
    }
    delay(1000);
}
```

See [examples/arduino_epdinky](../../examples/arduino_epdinky) for a complete,
buildable project using `espressif/arduino-esp32` as a managed component.

**`Serial` must not be UART0 on this board.** UART0 is GPIO37/GPIO38 — the
TPS65185 `WAKEUP` and `INT` lines — so the example defines
`ARDUINO_USB_CDC_ON_BOOT=1` and `ARDUINO_USB_MODE=1` build-wide to point
`Serial` at the USB Serial/JTAG CDC device.

The wrapper intentionally exposes no Arduino `String` members: the BSP is
compiled once without `Arduino.h`, so members conditional on `ARDUINO` would
give the class different definitions in different translation units and fail to
link.

## Status

Implemented: I2C core + scanner, TCA6408, TPS65185, KXTJ3, RV3028, STC3115,
EPD GPIO parking, user button, SD card mount/unmount, Wi-Fi (scan/connect),
BLE (scan), Arduino C++ wrapper.

Not yet implemented: EPD waveform/panel driver.
