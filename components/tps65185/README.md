# tps65185 — TPS65185 E Ink PMIC driver

Register-level I2C driver for the TI **TPS65185** (and pin/register-compatible
**TPS65186**) PMIC — the part that turns a single supply rail into everything
a raw E Ink panel needs: the negative-going programmable **VCOM** common-plane
voltage, and the **VDDH** (+22 V gate boost), **VPOS**/**VNEG** (±12–15 V
source rails) and **VEE** (−20 V gate rail) power rails. It is a hard
dependency of the [`epd`](../epd/README.md) raw-panel driver and is brought up
by every example in this repo that drives a panel.

- Full register-level access: `read`/`write`/`modify_register` plus named
  constants for all 17 registers and their bitfields
- VCOM: 0 to −5.11 V in 10 mV steps (9-bit DAC), Hi-Z control, one-time NVM
  programming
- VPOS/VNEG magnitude select (12/13/14/15 V)
- Configurable per-rail strobe order and inter-strobe delay for both
  power-up (`UPSEQ0`/`UPSEQ1`) and power-down (`DWNSEQ0`/`DWNSEQ1`) sequencing
- Power-good check via the `PG` register (per-rail detail) or an optional
  `PWRGOOD` GPIO
- Thermistor read (raw and converted) — the input the `epd` component uses
  for temperature-compensated waveform timing
- Optional hardware control of `WAKEUP`/`PWRUP`/`VCOM_CTRL`/`INT`/`PWRGOOD`;
  every affected function falls back to pure I2C when a pin isn't wired
- `tps65185_dump_registers()` / `tps65185_vcom_diagnostic()` debug helpers

Requires **ESP-IDF ≥ 5.5** (`idf_component.yml`) for the `driver/i2c_master.h`
I2C master API. No chip restrictions — this is a pure I2C + GPIO driver, not
tied to any particular ESP32 variant. Fixed 7-bit I2C address **0x68**,
400 kHz bus clock — both hardcoded in `tps65185_init()`, not configurable
through the API.

---

## Dependencies

| Component | Why |
|---|---|
| `esp_driver_i2c` | `i2c_master.h` — the I2C bus/device handles all register access goes through |
| `esp_driver_gpio` | `WAKEUP`/`PWRUP`/`VCOM_CTRL`/`INT`/`PWRGOOD` pin control |

---

## Layout

```
tps65185/
├── include/
│   └── tps65185.h   register map, bitfields, GPIO config struct, public API
└── tps65185.c        driver implementation
```

---

## Power states

Three states, not two — mixing these up either strands VCOM in a bad place
or makes the device unreachable over I2C when you didn't mean to:

| State | How | Rails | I2C |
|---|---|---|---|
| **Active** | default after `tps65185_wakeup()` | on if `tps65185_power_up()` was called | yes |
| **Standby** | `tps65185_standby()` — sets `ENABLE.STANDBY` over I2C | off | yes — still register-accessible |
| **Sleep** | `tps65185_sleep()` — de-asserts the `WAKEUP` GPIO | off | **no** — the I2C interface itself powers down |

Sleep can only be entered through the `WAKEUP` pin — there is no I2C-only
path — so `tps65185_sleep()` returns `ESP_ERR_NOT_SUPPORTED` when no
`wakeup_pin` was configured. Once asleep, registers are unreachable until
`tps65185_wakeup()` re-asserts `WAKEUP`; `tps65185_init()` also does this on
startup, waiting 10 ms before its first register read.

`tps65185_power_down()` (rails off, device still Active or moved to Standby
by a following call) and going all the way to Sleep are deliberately separate
calls. The `epd` component's `epd_panel_power_off()` always leaves the PMIC in
Standby, never Sleep — dropping `WAKEUP` is a whole-system decision (are you
about to touch this PMIC again soon, or is the MCU also going to deep sleep?)
that the display driver doesn't get to make for the application.

---

## Usage

```c
#include "tps65185.h"

static const tps65185_gpio_config_t pmic_gpio = {
    .wakeup_pin    = GPIO_NUM_37,   // WAKEUP: must be driven high before the
                                    // device answers on I2C at all
    .pwrup_pin     = GPIO_NUM_36,   // PWRUP: optional, triggers the HW sequencer
    .vcom_ctrl_pin = GPIO_NUM_35,   // VCOM_CTRL: optional, GPIO overrides I2C VCOM_EN
    .int_pin       = GPIO_NUM_38,   // INT: optional, active-low
    .pwr_good_pin  = GPIO_NUM_39,   // PWRGOOD: optional, open-drain — needs a pull-up
};

tps65185_handle_t pmic;
ESP_ERROR_CHECK(tps65185_init(i2c_bus, &pmic_gpio, &pmic));

// VPOS/VNEG magnitude - must match what the panel's own boost/charge-pump
// design expects (12-15 V in 1 V steps; see tps65185_vset_t).
ESP_ERROR_CHECK(tps65185_set_vpos_vneg(pmic, TPS65185_VSET_15V));

// VCOM is a property of the *individual panel*, printed on its FPC ribbon -
// not a per-model constant. 1500 -> -1.500 V.
ESP_ERROR_CHECK(tps65185_set_vcom(pmic, 1500));

ESP_ERROR_CHECK(tps65185_power_up(pmic));   // polls PG internally; see below
ESP_ERROR_CHECK(tps65185_vcom_enable(pmic, true));

// tps65185_power_up() does not fail its return code on a PG timeout (see
// Gotchas) - check explicitly if the caller needs a hard signal.
bool good = false;
tps65185_is_power_good(pmic, &good);
if (!good) {
    uint8_t pg = 0;
    tps65185_get_power_good_status(pmic, &pg);
    ESP_LOGW(TAG, "PMIC rails not good, PG=0x%02X", pg);
}

// ... drive the panel via the epd component ...

ESP_ERROR_CHECK(tps65185_power_down(pmic));
ESP_ERROR_CHECK(tps65185_standby(pmic));
tps65185_sleep(pmic);   // drops WAKEUP - only if nothing else needs the PMIC
```

In this repo the init/VCOM/rail steps above are normally done for you by
`bsp_tps65185_init()` in `components/epdinky_p4_board`, and the per-refresh
power-up/power-down sequence (including per-panel strobe ordering) is done by
`epd_panel_power_on()`/`epd_panel_power_off()` in the `epd` component — see
that component's [README](../epd/README.md) and
[EINK_DETAILS.md §4/§9](../epd/EINK_DETAILS.md) for exactly how it calls into
this driver. Application code that only uses `epd` never has to call most of
the functions above directly; the main one applications *do* call themselves
is `tps65185_sleep()`, on the way into MCU deep sleep — see
`examples/idf_epd_ha_firmware/main/ha_sleep.c` and
`examples/trmnl-firmware/main/trmnl_sleep.c`.

---

## Gotchas / bring-up notes

**`tps65185_power_up()` does not report a rail failure via its return code.**
It writes the `ENABLE` register, then polls `PG` every 10 ms for up to
800 ms. If the rails come good it returns `ESP_OK`; if the timeout expires it
logs `ESP_LOGW` naming exactly which rail(s) are still missing (`VB`/`VDDH`/
`VN`/`VPOS`/`VEE`/`VNEG`) — and **still returns `ESP_OK`**. It only returns a
non-`ESP_OK` code if the initial register write or a `PG` read itself fails
at the I2C level. `ESP_ERROR_CHECK(tps65185_power_up(...))` alone will not
catch a panel that never powers up — call `tps65185_is_power_good()` or
`tps65185_get_power_good_status()` afterward if the caller needs a hard
pass/fail signal. `epd_display.c` does exactly this: it runs its own
independent PG poll loop after calling `tps65185_power_up()`.

**Rail timing is uneven — don't replace the poll with a fixed delay.**
Measured on an epdInky board with an ED103TC2 panel: `VPOS`/`VEE`/`VNEG` read
good about 150 ms after `PWRUP`, but `VDDH` not until roughly 300 ms. A single
check at 100 ms reported a healthy panel as faulty.

**`PWR_GOOD` is open-drain and needs a pull-up.** The epdInky board wires it
straight to the GPIO with no resistor fitted, so `tps65185_init()` enables the
ESP32's internal pull-up on `pwr_good_pin` whenever one is configured.
Without it the pin floats/reads low permanently regardless of rail health.
Prefer reading the `PG` register (`tps65185_get_power_good_status()`) over the
pin when you need to know *which* rail failed — the register names each rail
individually and doesn't depend on a board having fitted that pull-up.

**Standby vs. Sleep are not interchangeable.** See [Power states](#power-states)
above — mixing them up either leaves the device unreachable over I2C
(Sleep, when you wanted Standby) or leaves the WAKEUP-gated hardware alive
and drawing current when you meant to fully power it down (Standby, when you
wanted Sleep).

**`tps65185_power_down()`'s sequencing is fixed, not parameterized.** Unlike
power-up (whose strobe order/delay is driven by the caller — the `epd`
component derives it from the panel definition), `tps65185_power_down()`
always programs the same `DWNSEQ0` (VDDH 3 ms, VPOS 6 ms, VEE 12 ms,
VNEG 9 ms) and `DWNSEQ1 = 0xE0` on every call. If that sequence-config I2C
write fails, the function does **not** abort — it proceeds to cut the rails
anyway (via the `PWRUP` GPIO if configured, otherwise by clearing the
`ENABLE` rail bits over I2C), on the reasoning that leaving rails powered
because a non-essential I2C write failed is worse than an unoptimized
ramp-down.

**`UPSEQ0`/`DWNSEQ0` hardware field layout:** `VDDH` occupies bits `[7:6]`,
`VPOS` bits `[5:4]`, `VEE` bits `[3:2]`, `VNEG` bits `[1:0]` — same order in
both the up- and down-sequence registers. `tps65185_set_powerup_sequence()`/
`tps65185_set_powerdown_sequence()` take one `tps65185_strobe_t` (3/6/9/12 ms)
per rail and pack this layout for you; there's no dedicated setter for the
`DFCTR`/`DLY` fields in `UPSEQ1`/`DWNSEQ1` — pack and
`tps65185_write_register()` them directly, as `epd_display.c` and
`tps65185_power_down()` itself both do.

**VCOM resolution truncates, and clamps silently.** `tps65185_set_vcom()`
converts millivolts to the 9-bit register with integer division by 10 —
values that aren't an exact multiple of 10 mV round down, with no error.
Requests above `TPS65185_VCOM_MAX_MV` (5110) are clamped to 5110 with an
`ESP_LOGW`, not rejected.

**No driver-level VCOM auto-acquisition.** The `VCOM2.ACQ` bit (start an
automatic VCOM read-back cycle) is defined in the header and visible via
`tps65185_vcom_diagnostic()`/`tps65185_read_register()`, but no function in
this driver sets it — VCOM is only ever set explicitly with
`tps65185_set_vcom()`, optionally followed by `tps65185_vcom_program_nvm()`.

**`ENABLE` readback after `tps65185_power_up()` normally differs from what
was written**, and that's expected rather than a fault: when a `PWRUP` GPIO
is wired, the PMIC's own hardware sequencer drives those bits once triggered,
so the register reflects the state machine's progress, not the last I2C
write. Logged at `ESP_LOGD` for tracing.

**Thermistor reads are not validated against conversion status.**
`tps65185_read_temperature()` sets `TMST1.READ_THERM`, waits a fixed 15 ms,
then casts the raw `TMST_VALUE` register straight to `int8_t` — it does not
check `TMST2.CONV_END`. A missing, open, or shorted thermistor can return a
railed value (e.g. `-128`); callers doing temperature compensation should
sanity-check the result against a plausible range before trusting it (the
`epd` component rejects anything outside `[-25, 85] °C` and falls back to
nominal timing).

---

## API reference

| Function | Purpose |
|---|---|
| `tps65185_init(bus, gpio_cfg, *handle)` | Add the device to an existing I2C bus, configure any GPIOs given (`-1`/`GPIO_NUM_NC` = unused), assert `WAKEUP` if configured, and verify the part by reading `REVID`. |
| `tps65185_deinit(handle)` | Power down, standby, drop `WAKEUP` if configured, remove the I2C device, free the handle. |
| `tps65185_get_revid(handle, *rev_id)` | Read the `REVID` register (0x10). |
| `tps65185_wakeup(handle)` | Assert `WAKEUP` (if configured) and clear `ENABLE.STANDBY`. |
| `tps65185_standby(handle)` | Set `ENABLE.STANDBY` — rails off, device stays I2C-reachable. |
| `tps65185_sleep(handle)` | De-assert `WAKEUP` — full shutdown including I2C. `ESP_ERR_NOT_SUPPORTED` if no `wakeup_pin` was configured. |
| `tps65185_power_up(handle)` | Enable all rails (`V3P3`, `VNEG`, `VEE`, `VPOS`, `VDDH`, `VCOM`) via `ENABLE`, assert `PWRUP` if configured, poll `PG` up to 800 ms. See Gotchas — always returns `ESP_OK` once the write succeeds, even on a PG timeout. |
| `tps65185_power_down(handle)` | Program a fixed `DWNSEQ0`/`DWNSEQ1` ramp-down, then cut rails via `PWRUP` GPIO or `ENABLE` bits. Always proceeds even if the sequence write fails. |
| `tps65185_is_power_good(handle, *power_good)` | `true`/`false` from the `PWRGOOD` pin if configured, else `PG.PG_ALL`. |
| `tps65185_get_power_good_status(handle, *pg_status)` | Raw `PG` register (0x0F) — per-rail detail. |
| `tps65185_set_vpos_vneg(handle, vset)` | Set `VADJ.VSET` — `TPS65185_VSET_15V/14V/13V/12V`. |
| `tps65185_set_vcom(handle, vcom_mv)` | Write `VCOM1`/`VCOM2` for `-vcom_mv` mV, 10 mV steps, clamped to 5110. |
| `tps65185_get_vcom(handle, *vcom_mv)` | Read back the current VCOM setting in mV. |
| `tps65185_vcom_enable(handle, enable)` | Drive `VCOM_CTRL` GPIO if configured, else toggle `ENABLE.VCOM_EN` over I2C. |
| `tps65185_vcom_hiz(handle, hiz)` | Set/clear `VCOM2.HIZ` (VCOM output high-impedance). |
| `tps65185_vcom_program_nvm(handle)` | Set `VCOM2.PROG`, wait 150 ms, check `INT1.PRGC`. Programs the current VCOM setting to NVM as the power-on default. |
| `tps65185_read_temperature(handle, *temperature)` | Trigger a thermistor conversion (`TMST1.READ_THERM`), wait 15 ms, return `TMST_VALUE` as `int8_t` °C. See Gotchas re: conversion-complete not checked. |
| `tps65185_read_thermistor_raw(handle, *raw_value)` | Read `TMST_VALUE` directly, no conversion trigger or wait. |
| `tps65185_set_powerup_sequence(handle, reg, vddh, vpos, vee, vneg)` | Pack four `tps65185_strobe_t` values into `UPSEQ0` (or another register passed via `reg`). |
| `tps65185_set_powerdown_sequence(handle, reg, vddh, vpos, vee, vneg)` | Same, for `DWNSEQ0`. |
| `tps65185_enable_interrupts(handle, int_en1, int_en2)` | Write `INT_EN1`/`INT_EN2` directly (raw bitmasks — see header for bit names). |
| `tps65185_read_interrupts(handle, *int1, *int2)` | Read `INT1`/`INT2` (either pointer may be `NULL`); reading clears the flags. |
| `tps65185_read_register(handle, reg, *value)` | Raw single-register read. |
| `tps65185_write_register(handle, reg, value)` | Raw single-register write. |
| `tps65185_modify_register(handle, reg, mask, value)` | Read-modify-write a subset of bits. |
| `tps65185_dump_registers(handle)` | `ESP_LOGI` every named register (0x00–0x10). |
| `tps65185_vcom_diagnostic(handle)` | `ESP_LOGI`/`ESP_LOGW` a human-readable decode of `ENABLE`, `VADJ`, `VCOM1`/`VCOM2` — flags VCOM disabled, Hi-Z, or 0 V as likely misconfiguration. |

All registers, bitfields, enums (`tps65185_strobe_t`, `tps65185_delay_t`,
`tps65185_vset_t`) and the `tps65185_gpio_config_t` struct are documented
with their bit positions in `include/tps65185.h`; that header is the
authoritative reference for anything not covered above.
