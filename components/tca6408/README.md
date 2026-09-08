# tca6408 — TCA6408 I2C GPIO expander driver

Driver for the Texas Instruments **TCA6408(A)**, an 8-bit I2C GPIO expander
with per-pin direction and polarity-inversion registers. It's a thin,
general-purpose register wrapper — this repo instantiates it three times on
one board, each time for something unrelated: SD-card power switching and
sensor interrupt fan-in on the mainboard, and panel power/reset/backlight
sequencing on the MIPI-DSI display adapter. See [Used for](#used-for-on-this-board)
below.

- Opaque handle (`tca6408_handle_t`) over ESP-IDF's `i2c_master` driver — one
  handle per physical expander, so two expanders at different addresses on
  the same bus just need two `tca6408_init()` calls
- Whole-port reads/writes (`tca6408_get_input_val`, `tca6408_set_output_val`,
  `tca6408_set_config`, `tca6408_set_polarity`) plus a single-pin output
  helper (`tca6408_set_output_pin`) that read-modify-writes a cached copy of
  the output latch
- No interrupt handling — the TCA6408's open-drain `INT` pin is a plain
  ESP32 GPIO input read with `esp_driver_gpio`/`gpio_get_level()` elsewhere;
  this component only talks to the expander over I2C

Requires **ESP-IDF ≥ 5.5** (uses `driver/i2c_master.h`, the `i2c_master_bus_handle_t` /
`i2c_master_dev_handle_t` API).

---

## Dependencies

| Component | Why |
|---|---|
| `esp_driver_i2c` | `i2c_master_transmit`/`i2c_master_transmit_receive` over an existing bus handle |
| `esp_driver_gpio` | declared in `CMakeLists.txt`'s `REQUIRES`, but the driver itself makes no GPIO calls — the expander's `INT` line is wired up by the caller, not this component |

This component does not create or own an I2C bus — you pass in an
`i2c_master_bus_handle_t` that something else (usually the board's
`bsp_i2c_init()`/`bsp_i2c_get_handle()`) already created.

---

## Layout

```
tca6408/
├── include/
│   └── tca6408.h   register addresses, tca6408_handle_t, public API
└── tca6408.c        i2c_master transactions, output-latch cache
```

---

## Usage

```c
#include "tca6408.h"

i2c_master_bus_handle_t bus;   // from bsp_i2c_get_handle() or your own I2C setup

tca6408_handle_t exp;
ESP_ERROR_CHECK(tca6408_init(bus, 0x21, &exp));

// Direction: 1 = input, 0 = output (per-bit, TCA6408_CONFIG_REG).
// Make P7 an output, everything else stays input.
ESP_ERROR_CHECK(tca6408_set_config(exp, 0x7F));

// Preload the output latch, then flip a single pin without disturbing
// the others.
ESP_ERROR_CHECK(tca6408_set_output_val(exp, 0x80));   // P7 high
ESP_ERROR_CHECK(tca6408_set_output_pin(exp, 7, 0));   // now P7 low

uint8_t inputs;
ESP_ERROR_CHECK(tca6408_get_input_val(exp, &inputs));  // reads P0..P6 live levels

tca6408_delete(exp);
```

`tca6408_init()` reads back the output register once at init time and caches
it in the handle, so `tca6408_set_output_pin()` can do a read-modify-write
purely in software (no I2C read before every single-pin change). Call
`tca6408_set_output_val()` at least once for the pins you care about before
relying on `tca6408_set_output_pin()`, since the cache starts from whatever
the chip's output register happens to power up as — it isn't guaranteed to
be all-zero.

`tca6408_set_config()` and `tca6408_set_polarity()` take a *direction* /
*polarity* bitmask respectively, both whole-port only — there's no per-pin
helper for either, unlike the output register.

---

## Bring-up notes

- **Direction bit convention.** `tca6408_set_config()`'s bitmask is **1 =
  input, 0 = output**, matching the TI datasheet's `CONFIG` register (this is
  also how every call site in this repo uses it — see
  `components/epdinky_p4_board/include/bsp/config.h`'s `BSP_TCA6408_DIR_DEFAULT`
  and the inline comments in `examples/idf_dsi_camera_preview/main/app_display.c`).
  Don't assume the opposite convention some other expander families use.
- **Sequence output value before direction when driving a load you don't
  want to glitch.** `bsp_tca6408_init()` in `components/epdinky_p4_board/epdinky_p4_board.c`
  writes the output latch (`tca6408_set_output_val`) *before* switching the
  relevant pin to an output (`tca6408_set_config`), specifically so the SD
  card's power-switch FET never briefly turns on during the direction change.
  The DSI adapter bring-up in `app_display.c`/`ha_panel_jd9168.c` does the
  same: `tca6408_set_output_val(exp, 0x00)` then `tca6408_set_config(...)`,
  so panel power/reset/backlight all start deasserted before becoming
  outputs.
- **`tca6408_init()` fails if the expander doesn't answer.** It probes the
  device with `i2c_master_bus_add_device()` and then reads the output
  register; both call sites for the DSI adapter's expander treat that failure
  as "adapter board not fitted" rather than a fatal error in some configurations.
- **Polarity inversion is rarely used here.** Every call site in this repo
  sets `tca6408_set_polarity(handle, 0x00)` (normal, non-inverted) or skips
  the call entirely — none of the boards rely on the `POLARITY_REG`'s
  inversion feature.

---

## Used for on this board

The epdInky ESP32-P4 board carries **two** independent TCA6408 instances at
different I2C addresses, plus a third possible instance depending on
adapter board fitted:

| Instance | Address | Where | Purpose |
|---|---|---|---|
| Mainboard expander | `0x21` (`I2C_DEVICE_TCA6408_ADDR`, `ADDR` strapped to VCC3V3) | `components/epdinky_p4_board/epdinky_p4_board.c` (`bsp_tca6408_init`) | Fans in the accelerometer (KXTJ3) and RTC (RV3028) interrupt lines on P0/P1 (inputs), exposes header J3 pins P2-P5 as spare inputs, and switches micro-SD card power via P7 (`bsp_sdcard_set_power`) through an AO3407 P-FET, **active low** |
| DSI adapter expander | `0x20` (`BSP_I2C_ADDR_LCD_EXPANDER`) | `examples/idf_dsi_camera_preview/main/app_display.c`, `examples/idf_ha_firmware/main/ha_panel_jd9168.c` | Gates the D320C2403V-MIPI panel adapter's power (P0 → AP2281 load switch → panel `LCD_VDD`), JD9168 hardware reset (P1, active low), and SGM37604A backlight enable (P2) |

Mainboard expander direction/output defaults
(`components/epdinky_p4_board/include/bsp/config.h`):

```c
#define BSP_TCA6408_DIR_DEFAULT  0x7F   // P0..P6 input, P7 output (SD power gate)
#define BSP_TCA6408_OUT_DEFAULT  0x80   // P7 high = card powered OFF (active-low switch)
```

`bsp_tca6408_init()` also configures a plain ESP32 GPIO
(`BSP_TCA6408_PIN_INT_IO`, GPIO 34) as an input for the expander's own
open-drain `INT` line — that pin is read directly with `gpio_get_level()`,
not through this driver, since the TCA6408 doesn't expose interrupt status
over I2C beyond "an input changed since the last `INPUT_REG` read."

DSI adapter bring-up order (from `ha_panel_jd9168.c`, and identically in
`app_display.c`) — **order matters**: the backlight IC doesn't ACK on I2C at
all until P2 is high, and the panel is completely unpowered (looks like a
missing adapter board on an I2C scan) until P0 is high.

```c
tca6408_handle_t exp;
ESP_ERROR_CHECK(tca6408_init(bus, BSP_I2C_ADDR_LCD_EXPANDER, &exp));

// Preset everything low/off, then make P0..P2 outputs.
ESP_ERROR_CHECK(tca6408_set_output_val(exp, 0x00));
ESP_ERROR_CHECK(tca6408_set_config(exp, (uint8_t)~((1 << BSP_LCD_EXP_PIN_POWER_EN)  |
                                                    (1 << BSP_LCD_EXP_PIN_PANEL_RST) |
                                                    (1 << BSP_LCD_EXP_PIN_BACKLIGHT_EN))));

ESP_ERROR_CHECK(tca6408_set_output_pin(exp, BSP_LCD_EXP_PIN_POWER_EN, 1));  // panel power on
vTaskDelay(pdMS_TO_TICKS(20));                                             // let rails settle

ESP_ERROR_CHECK(tca6408_set_output_pin(exp, BSP_LCD_EXP_PIN_PANEL_RST, 0)); // assert reset
vTaskDelay(pdMS_TO_TICKS(10));
ESP_ERROR_CHECK(tca6408_set_output_pin(exp, BSP_LCD_EXP_PIN_PANEL_RST, 1)); // release reset
vTaskDelay(pdMS_TO_TICKS(120));                                            // JD9168 settle time

ESP_ERROR_CHECK(tca6408_set_output_pin(exp, BSP_LCD_EXP_PIN_BACKLIGHT_EN, 1)); // now it ACKs
vTaskDelay(pdMS_TO_TICKS(10));
ESP_ERROR_CHECK(sgm37604a_init(bus, SGM37604A_CURRENT_30MA));
```

Since panel reset here is an expander register write rather than an ESP32
GPIO, the JD9168 panel is created with `reset_gpio_num = -1` and
`esp_lcd_panel_reset()` is skipped — the hardware reset above already
happened, and a DCS software reset at that point would just undo it right
before the init sequence runs. See `components/esp_lcd_jd9168/README.md`
for the full panel bring-up.

---

## API reference

| Function | Purpose |
|---|---|
| `tca6408_init(bus_handle, address, *ret_handle)` | Add the device to an existing I2C bus (`i2c_master_bus_add_device`, 400 kHz, 7-bit address) and cache its current output register. Fails if the device doesn't ACK or the initial output-register read fails. |
| `tca6408_delete(handle)` | Remove the device from the bus and free the handle. |
| `tca6408_set_output_val(handle, val)` | Write `OUTPUT_REG` (all 8 pins at once) and update the cached output latch. |
| `tca6408_set_output_pin(handle, pin, level)` | Read-modify-write a single output pin (0-7) using the cached latch, then write `OUTPUT_REG`. Returns `ESP_ERR_INVALID_ARG` for `pin` outside 0-7. |
| `tca6408_get_input_val(handle, *val)` | Read `INPUT_REG` — live pin levels, regardless of direction. |
| `tca6408_set_config(handle, val)` | Write `CONFIG_REG` — per-bit direction, **1 = input, 0 = output**. |
| `tca6408_set_polarity(handle, val)` | Write `POLARITY_REG` — per-bit input polarity inversion, **1 = inverted, 0 = normal**. Affects only what `tca6408_get_input_val()` reports, not the physical pin. |

Register addresses (`tca6408.h`): `TCA6408_INPUT_REG` `0x00`,
`TCA6408_OUTPUT_REG` `0x01`, `TCA6408_POLARITY_REG` `0x02`,
`TCA6408_CONFIG_REG` `0x03`.
