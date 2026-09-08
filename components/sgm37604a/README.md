# sgm37604a — SGM37604A I2C LED/backlight driver

Driver for the **SGM37604A**, an I2C-controlled LED driver IC. In this repo it
drives the backlight on the **D320C2403V-MIPI** display module (the JD9168
MIPI-DSI panel's adapter board) — there is no GPIO brightness control on that
board, so this I2C path is the only way to light the panel.

- 12-bit brightness control (`0`–`SGM37604A_MAX_BRIGHTNESS`, i.e. 4095), plus
  a `0`–`100` percentage convenience wrapper
- Selectable full-scale LED current (25/30/35/40 mA), which caps the maximum
  achievable brightness
- Enable/disable the LED string independently of the brightness setting
- Fault-flag read-back, to tell a broken LED string from a working backlight
  showing the wrong image
- Probes the device before attaching it, so a disconnected display module is
  reported as one clear error rather than a run of failed register writes
- Leaves the backlight at zero brightness after `init` — the caller raises it
  once there is something on screen, avoiding a bright flash at startup

Requires ESP-IDF's I2C master driver (`driver/i2c_master.h`) and an
`i2c_master_bus_handle_t` for a bus already installed by the caller (e.g. from
a BSP's `bsp_i2c_get_handle()`). Developed and verified on **ESP32-P4**
(epdInky board + D320C2403V-MIPI adapter).

---

## Dependencies

| Component | Why |
|---|---|
| `driver` | `i2c_master_bus_handle_t` / `i2c_master_dev_handle_t` types |
| `esp_driver_i2c` | I2C master probe, add-device, transmit/transmit-receive |

Not linked by this component, but needed to bring the chip up on hardware
that gates its hardware-enable pin through an I2C expander (like the epdInky
D320C2403V-MIPI adapter — see [Bring-up notes](#bring-up-notes) below):

| Component | Why |
|---|---|
| `tca6408` | GPIO expander driving the SGM37604A's `HWEN` pin (and the panel's power/reset) |

There is no `idf_component.yml` in this component — its only dependencies are
the two IDF components above, declared via `REQUIRES` in `CMakeLists.txt`.

---

## Layout

```
sgm37604a/
├── include/
│   └── sgm37604a.h   public API: init/deinit, brightness, enable, fault read
└── sgm37604a.c        register writes and I2C transactions
```

---

## Usage

```c
#include "sgm37604a.h"

// The backlight IC sits behind a TCA6408 I/O expander on this board: P2
// drives HWEN, and the chip will not ACK on I2C at all until it is high.
// (See the full bring-up in examples/idf_ha_firmware/main/ha_panel_jd9168.c
// and examples/idf_dsi_camera_preview/main/app_display.c.)
ESP_ERROR_CHECK(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_BACKLIGHT_EN, 1));
vTaskDelay(pdMS_TO_TICKS(10));

ESP_ERROR_CHECK(sgm37604a_init(bus, SGM37604A_CURRENT_30MA));

// Later, once a frame is actually on screen:
sgm37604a_set_brightness_percent(80);   // 0-100
```

`sgm37604a_init()` probes address `0x36` before attaching the device, so if
the display module (or its adapter) is unplugged, `init` fails immediately
with a clear log message instead of every subsequent register write failing
silently one at a time.

`init` also programs `LED_ENABLE`, `MODE` and the `max_current` current limit,
then explicitly sets brightness to `0` — the caller is expected to raise it
only once a real frame is on the panel, so there's no bright flash of a stale
or blank buffer at power-on.

The driver keeps a single static device handle internally (there is no handle
parameter on any call after `init`), so only one SGM37604A can be attached at
a time. Calling `sgm37604a_init()` again while already attached reuses the
existing device and just re-programs the registers.

---

## Bring-up notes

**The chip will not respond on I2C until `HWEN` is driven high.** On the
epdInky D320C2403V-MIPI adapter, `HWEN` is wired to pin P2 of a TCA6408 I/O
expander at `0x20` (see `examples/idf_dsi_camera_preview/main/app_display.c`
and `examples/idf_ha_firmware/main/ha_panel_jd9168.c` for the full sequence).
Until that pin is driven high, the SGM37604A does not appear on an I2C bus
scan at all — indistinguishable, at the bus level, from the display module
not being fitted. The working order on that board is: power the panel, let
its rails settle, release panel reset, *then* drive `HWEN` high, *then* call
`sgm37604a_init()`.

**Brightness is 12-bit, split across two registers — get the split right.**
`SGM37604A_REG_BRIGHTNESS_LSB` (`0x1A`) takes the low nibble (`level & 0x0F`)
and `SGM37604A_REG_BRIGHTNESS_MSB` (`0x19`) takes the top 8 bits
(`(level >> 4) & 0xFF`). Writing the level straight into the MSB register
without shifting truncates it to 8 bits instead — on the panel this looks
like the backlight wrapping and dimming again every time the level crosses a
multiple of 256. `sgm37604a_set_brightness()` already does this correctly;
this is only a trap for anyone writing the registers directly.

**Fault flags.** `sgm37604a_get_faults()` reads `SGM37604A_REG_FAULT_FLAGS`
(`0x1F`). `0` means no faults; a nonzero value indicates an LED-string fault
(open or shorted), which is worth checking during bring-up to distinguish "the
backlight is broken" from "the backlight is fine but the image is wrong". The
individual fault bit meanings are not broken out by this driver — treat any
nonzero value as "investigate the LED string".

**Current setting caps brightness, it isn't a separate dimmer.** The
`sgm37604a_current_t` value passed to `init` (25/30/35/40 mA) sets the
full-scale LED current via `SGM37604A_REG_CURRENT` (`0x1B`); brightness above
that is then a PWM/ramp duty cycle within that current limit, set separately
via the two brightness registers.

---

## API reference

| Symbol | Purpose |
|---|---|
| `sgm37604a_init(bus, max_current)` | Probe the device at `0x36`, attach it to `bus`, program `LED_ENABLE`/`MODE`/current limit, and zero the brightness. Idempotent if already attached. |
| `sgm37604a_set_brightness(level)` | Set raw 12-bit brightness, `0`–`SGM37604A_MAX_BRIGHTNESS` (4095). Values above the max are clamped. |
| `sgm37604a_set_brightness_percent(percent)` | Set brightness as `0`–`100`, scaled to the 12-bit range. Values above 100 are clamped. |
| `sgm37604a_enable(bool on)` | Turn the LED string fully on or off without touching the stored brightness level. |
| `sgm37604a_get_faults(uint8_t *flags)` | Read the fault-flags register into `*flags`. `0` = no faults. |
| `sgm37604a_deinit(void)` | Disable the backlight and remove the device from the I2C bus. Safe to call if never initialised. |

### Constants

| Symbol | Value | Meaning |
|---|---|---|
| `SGM37604A_I2C_ADDR` | `0x36` | Fixed 7-bit I2C address |
| `SGM37604A_I2C_SPEED_HZ` | `400000` | SCL speed used when the device is added to the bus |
| `SGM37604A_MAX_BRIGHTNESS` | `4095` | Top of the 12-bit brightness range |
| `SGM37604A_REG_LED_ENABLE` | `0x10` | Enables/disables the LED string |
| `SGM37604A_REG_MODE` | `0x11` | Ramp/PWM mode select |
| `SGM37604A_REG_BRIGHTNESS_MSB` | `0x19` | Top 8 bits of the 12-bit brightness level |
| `SGM37604A_REG_BRIGHTNESS_LSB` | `0x1A` | Low nibble (bits 0–3) of the 12-bit brightness level |
| `SGM37604A_REG_CURRENT` | `0x1B` | Full-scale LED current select |
| `SGM37604A_REG_FAULT_FLAGS` | `0x1F` | Fault status; `0` = no faults |

`sgm37604a_current_t`: `SGM37604A_CURRENT_25MA` (`0x00`), `_30MA` (`0x01`),
`_35MA` (`0x02`), `_40MA` (`0x03`).
