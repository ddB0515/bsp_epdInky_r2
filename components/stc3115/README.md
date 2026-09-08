# stc3115 — STC3115 I2C battery gas gauge driver

Driver for the STMicroelectronics **STC3115**, a mixed coulomb-counting /
voltage-mode Li-ion fuel gauge IC on I2C (7-bit address `0x70`). It follows
the chip's official bring-up sequence — verify ID, check the 16-byte on-chip
RAM for a previously saved state, and either restore from it or run a full
init — and layers a software SOC estimate and a simple charge-status
heuristic on top of the chip's own registers.

- Full init/restore sequence: reads the ID register (expects `0x14`), checks
  RAM0–RAM15 for a valid test word (`0x53`) and CRC-8, and only performs a
  full re-init when RAM is invalid or the chip's own `BATFAIL`/`PORDET` flags
  are set — otherwise it writes the saved SOC/OCV/CC_ADJ/VM_ADJ straight back
  and resumes, avoiding a fresh relaxation/coulomb-counting run on every boot
- Mixed mode (coulomb counting via an external sense resistor) or pure
  voltage mode, selected per device with `stc3115_config_t::voltage_mode`
- `CC_CNF`/`VM_CNF` gas-gauge scaling computed from battery capacity (mAh)
  and sense resistor (mOhm) at init time — the only "battery model" input
  besides the 16-entry OCV table
- SOC and voltage alarm thresholds, with `BATFAIL` (UVLO < 2.6 V) and
  `PORDET` (POR < 2 V) status flags surfaced in `stc3115_data_t`
- `stc3115_save_state()` / RAM-backed restore so SOC survives a reset
- `stc3115_read_data()` restarts the gas gauge on its own if it observes
  `BATFAIL` or a cleared `GG_RUN` bit — a workaround for sense-resistor noise
  (see comment in `stc3115_read_data()`)
- A software, voltage-based SOC estimate and a 4-sample voltage-trend charge
  heuristic, both layered on top of the raw registers — see
  [Bring-up notes](#bring-up-notes) before trusting either blindly

Requires **ESP-IDF ≥ 5.5** with the new `i2c_master` driver
(`driver/i2c_master.h`). Developed and verified on **ESP32-P4** (epdInky
board, one STC3115 at `0x70` on the shared I2C bus).

---

## Dependencies

| Component | Why |
|---|---|
| `esp_driver_i2c` | `i2c_master.h` — bus/device transactions |
| `esp_driver_gpio` | listed in `CMakeLists.txt` but unused — the source never includes `gpio.h` or touches a GPIO. The STC3115's `ALM` alert pin is pulled to VBAT and not routed to the MCU on this board (see `epdinky_p4_board/include/bsp/config.h`), so the chip is polled, never interrupt-driven. Likely a leftover dependency. |

---

## Layout

```
stc3115/
├── include/
│   └── stc3115.h   registers, bitmasks, config/data structs, public API
└── stc3115.c        init/restore sequence, register I/O, SOC/voltage math
```

---

## Usage

Adapted from `bsp_stc3115_init()` in
`components/epdinky_p4_board/epdinky_p4_board.c`, the only place in this repo
that calls `stc3115_init()`:

```c
#include "stc3115.h"

stc3115_config_t gauge_cfg = {
    .battery_capacity_mah = 500,   // mAh
    .sense_resistor_mohm  = 10,    // mOhm
    .alarm_soc            = 0,     // 0 = alarm disabled
    .alarm_voltage_mv     = 0,     // 0 = alarm disabled
    .current_thres        = STC3115_DEFAULT_CURRENT_THRES,  // 10
    .relax_max            = STC3115_DEFAULT_RELAX_MAX,      // 24
    .voltage_mode         = false, // false = mixed mode (coulomb counting + voltage)
    .ocv_table            = NULL,  // NULL = driver's built-in default table — see below
};

stc3115_handle_t handle = NULL;
stc3115_init_status_t status;
ESP_ERROR_CHECK(stc3115_init(i2c_bus, &gauge_cfg, &handle, &status));
// status is STC3115_INIT_NEW (fresh init) or STC3115_INIT_RESTORED (resumed
// from valid on-chip RAM)

ESP_ERROR_CHECK(stc3115_start(handle));
```

Reading, adapted from `examples/idf_epd_ha_firmware/main/ha_battery.c` and
`examples/trmnl-firmware/main/trmnl_battery.c` (both wrap the handle the BSP
already brought up — `stc3115_init()`/`stc3115_start()` are not called
again):

```c
stc3115_data_t data;
esp_err_t err = stc3115_read_data(handle, &data);

float voltage_v = data.voltage_mv / 1000.0f;
float percent    = data.soc_permille / 10.0f;   // 0.1% units -> percent
bool  charging    = (data.charge_status == STC3115_CHARGING ||
                      data.charge_status == STC3115_FULLY_CHARGED);

// This board has no VBUS/USB-detect pin, so "is a battery even connected"
// is derived, not measured directly: BATFAIL (UVLO < 2.6 V) backed by a
// plausibility floor on voltage in case BATFAIL is ever wrong.
bool present = !data.battery_fail && data.voltage_mv >= 2000;
```

Both real call sites treat a non-`ESP_OK` return as "no reading" (they zero
their own status struct first) rather than propagating the error loudly,
since a transient I2C failure on a polled, non-critical sensor shouldn't
block the rest of the app.

At shutdown, `stc3115_delete()` already calls `stc3115_save_state()` and
`stc3115_stop()` for you — no need to call `stc3115_save_state()` separately
unless you want a checkpoint before that.

---

## Battery model / configuration

The driver takes only two capacity-related inputs — there is no external
impedance/aging table:

- **`battery_capacity_mah` and `sense_resistor_mohm`** — used at
  `stc3115_init()` time to compute the gas gauge's `CC_CNF`/`VM_CNF`
  registers: `CC_CNF = (capacity_mAh × sense_mOhm × 250) / 4096`, and
  `VM_CNF` is set to the same value.
- **`ocv_table`** — a pointer to a 16-entry, 16-register (`0x30`–`0x3F`) open
  circuit voltage curve used by the chip's own relaxation SOC logic. Per
  `stc3115.h`: `OCV(mV) = value × 5.5 + 2500`, so this 8-bit table can only
  represent up to `255 × 5.5 + 2500 = 3902 mV` — it cannot express a fully
  charged 4.2 V Li-ion cell.

**The header's `STC3115_DEFAULT_OCV_TABLE` macro is dead code.** `stc3115.h`
defines a full Li-ion discharge curve (3.0 V–3.9 V) under that name, with
comments describing the plateau shape, but `stc3115.c` never references it.
The actual fallback used when `config.ocv_table == NULL` is:

```c
static const uint8_t stc3115_default_ocv_table[16] = {0}; // STC3115_DEFAULT_OCV_TABLE;
```

i.e. **all-zero**, which under the formula above decodes every table entry
to 2500 mV. The BSP's `bsp_stc3115_init()` passes `ocv_table = NULL`, so on
this board the chip's internal relaxation SOC table is currently programmed
with all-zero entries rather than the documented curve. Pass an explicit
16-byte table (e.g. built from `STC3115_DEFAULT_OCV_TABLE`) if you need the
chip's own relaxation-mode SOC to be meaningful.

---

## Bring-up notes

**The final `soc_permille` from `stc3115_read_data()` is not the chip's gas
gauge value — it's a software linear interpolation over voltage.** After
parsing the raw SOC register (1/512 %-per-LSB, converted to permille),
`stc3115_read_data()` unconditionally overwrites `data->soc_permille` with a
linear interpolation between `STC3115_BATT_VOLTAGE_EMPTY` (3000 mV, 0%) and
`STC3115_BATT_VOLTAGE_FULL` (4200 mV, 100%). This sidesteps the OCV table's
~3.9 V ceiling described above, but it also means the chip's own
coulomb-counting/relaxation intelligence never reaches the caller through
this function. By contrast, **`stc3115_read_soc()` returns the raw hardware
SOC register value directly**, with no voltage override — the two APIs can
disagree, and `stc3115_read_data()`'s SOC is the one every real call site in
this repo (`ha_battery.c`, `trmnl_battery.c`) actually uses.

**Voltage conversion factor is a calibrated override, not the datasheet
value.** `STC3115_VOLTAGE_FACTOR` is `2.2` mV/LSB; the comment above it in
`stc3115.h` notes the datasheet states 2.44 mV/LSB and this value was
"calibrated based on actual measurements." If you're bringing this driver up
on different hardware, re-verify this constant against a known-good voltage
source before trusting `voltage_mv`.

**Current factor scales with sense resistor, calibrated at 10 mΩ.**
`STC3115_CURRENT_FACTOR` (5.88 µA/LSB) is documented as "with 10 mOhm sense
resistor"; `stc3115_init()` scales it for the configured resistor as
`current_factor = (5.88 × 10) / sense_resistor_mohm`.

**`stc3115_read_data()` restarts the gas gauge on `BATFAIL` or a cleared
`GG_RUN`,** clearing `CTRL`, writing `MODE = 0`, then rewriting `MODE` with
`GG_RUN | ALM_ENA` (plus `VMODE` or `FORCE_CC` per config) — every call, if
needed. The in-source comment calls this "a workaround for hardware noise on
the sense resistor."

**`stc3115_reset()` sets `GG_RST` and never clears it.** Compare this to the
soft-reset path `stc3115_start()` runs internally when it sees `BATFAIL`:
set `GG_RST`, delay, then explicitly clear it before reconfiguring. Calling
the public `stc3115_reset()` alone leaves the reset bit asserted in `CTRL`.

**Charge-status/voltage-trend state is function-local `static`, not
per-handle.** The 4-sample voltage history, sample counter and
`last_known_status` used to derive `stc3115_charge_status_t` inside
`stc3115_read_data()` are `static` variables inside that function — shared
across every `stc3115_handle_t`. Harmless with the single gas gauge this
board has, but `charge_status`/`voltage_trend` would not be correctly
isolated across two simultaneous STC3115 instances.

**RAM validity check:** `stc3115_is_ram_valid()` requires `RAM[0] == 0x53`
("test word") and a matching CRC-8 (polynomial `0x07`, computed over
`RAM[0..14]`, stored in `RAM[15]`) before trusting a saved state; either
mismatch triggers a full re-init.

**Alarm register resolution differs from the main voltage/SOC registers.**
`stc3115_set_alarm_soc()` uses 0.5%/LSB (`soc_percent × 2`);
`stc3115_set_alarm_voltage()` uses ~17.6 mV/LSB (`voltage_mv / 17.6`) — both
are single 8-bit registers, coarser than the 16-bit `SOC`/`VOLTAGE` register
pairs.

---

## Register map & constants

| Symbol | Address | Notes |
|---|---|---|
| `STC3115_REG_MODE` | `0x00` | `VMODE`(0) `CLR_VM_ADJ`(1) `CLR_CC_ADJ`(2) `ALM_ENA`(3) `GG_RUN`(4) `FORCE_CC`(5) `FORCE_VM`(6) |
| `STC3115_REG_CTRL` | `0x01` | `IO0DATA`(0) `GG_RST`(1) `GG_VM`(2) `BATFAIL`(3, UVLO<2.6V) `PORDET`(4, POR<2V) `ALM_SOC`(5) `ALM_VOLT`(6) |
| `STC3115_REG_SOC_L/H` | `0x02`/`0x03` | SOC, 1/512 %-per-LSB |
| `STC3115_REG_COUNTER_L/H` | `0x04`/`0x05` | Conversion counter |
| `STC3115_REG_CURRENT_L/H` | `0x06`/`0x07` | Signed battery current |
| `STC3115_REG_VOLTAGE_L/H` | `0x08`/`0x09` | Battery voltage, 2.2 mV/LSB (calibrated; see above) |
| `STC3115_REG_TEMPERATURE` | `0x0A` | `°C = raw - 30` |
| `STC3115_REG_AVG_CURRENT_L/H` | `0x0B`/`0x0C` | Average current, CC mode only |
| `STC3115_REG_OCV_L/H` | `0x0D`/`0x0E` | Open-circuit voltage |
| `STC3115_REG_CC_CNF_L/H` | `0x0F`/`0x10` | Coulomb-counting gauge config |
| `STC3115_REG_VM_CNF_L/H` | `0x11`/`0x12` | Voltage-mode gauge config |
| `STC3115_REG_ALARM_SOC` | `0x13` | 0.5 %/LSB |
| `STC3115_REG_ALARM_VOLTAGE` | `0x14` | ~17.6 mV/LSB |
| `STC3115_REG_CURRENT_THRES` | `0x15` | Relaxation current threshold (default `10`) |
| `STC3115_REG_RELAX_COUNT` / `RELAX_MAX` | `0x16`/`0x17` | Relaxation counter / max (default `24`) |
| `STC3115_REG_ID` | `0x18` | Expected `0x14` |
| `STC3115_REG_CC_ADJ_H/L`, `VM_ADJ_H/L` | `0x1B`–`0x1E` | Persisted adjustment values |
| `STC3115_REG_RAM0` | `0x20`–`0x2F` | 16-byte scratch RAM, see [Bring-up notes](#bring-up-notes) |
| `STC3115_REG_OCV_TAB0` | `0x30`–`0x3F` | 16-entry OCV table, `mV = value × 5.5 + 2500` |

I2C address: `STC3115_I2C_ADDRESS` = `0x70` (7-bit), opened at 400 kHz in
`stc3115_init()`.

---

## API reference

| Function | Purpose |
|---|---|
| `stc3115_init(bus_handle, config, &handle, &init_status)` | Probe ID, check on-chip RAM, and either restore or fully initialize. `init_status` (optional) reports `STC3115_INIT_NEW`/`STC3115_INIT_RESTORED`/`STC3115_INIT_FAILED`. |
| `stc3115_delete(handle)` | Save state to RAM, stop the gas gauge, remove the I2C device, free the handle. |
| `stc3115_start(handle)` | Set `MODE` (`GG_RUN`\|`ALM_ENA`, plus `VMODE` or `FORCE_CC`). Runs a `GG_RST` soft-reset sequence first if `BATFAIL` is set. |
| `stc3115_stop(handle)` | Write `MODE = 0x00` (does not save state — call `stc3115_save_state()` first if needed). |
| `stc3115_reset(handle)` | Set `GG_RST` in `CTRL`. Does not clear it afterward — see [Bring-up notes](#bring-up-notes). |
| `stc3115_read_data(handle, &data)` | Read CTRL/SOC/counter/current/voltage/temperature/avg-current in one burst, plus OCV separately; overwrites SOC with a voltage-based estimate and derives `charge_status`/`voltage_trend`. Restarts the gauge internally on `BATFAIL`/stopped `GG_RUN`. |
| `stc3115_read_voltage(handle, &voltage_mv)` | Single voltage register read. |
| `stc3115_read_current(handle, &current_ua)` | Single current register read (`int16_t`). |
| `stc3115_read_soc(handle, &soc_permille)` | Raw hardware SOC register, converted to permille — **not** the voltage-based estimate `stc3115_read_data()` returns. |
| `stc3115_read_temperature(handle, &temp_c)` | Single temperature register read. |
| `stc3115_set_alarm_soc(handle, soc_percent)` | Write `ALARM_SOC` (0.5 %/LSB). |
| `stc3115_set_alarm_voltage(handle, voltage_mv)` | Write `ALARM_VOLTAGE` (~17.6 mV/LSB). |
| `stc3115_clear_alarms(handle)` | Write `CTRL = 0x00`, clearing `ALM_SOC`/`ALM_VOLT`/`BATFAIL`/`PORDET` flags. |
| `stc3115_read_id(handle, &id)` | Read `STC3115_REG_ID`. |
| `stc3115_probe(handle)` | `ESP_OK` if the ID register reads back `STC3115_ID` (`0x14`). |
| `stc3115_save_state(handle)` | Read back CTRL/SOC/OCV/CC_ADJ/VM_ADJ and write them into on-chip RAM with a fresh CRC-8, for restoration on the next `stc3115_init()`. |

### Key types

| Type | Notes |
|---|---|
| `stc3115_config_t` | `battery_capacity_mah`, `sense_resistor_mohm`, `alarm_soc`, `alarm_voltage_mv`, `current_thres`, `relax_max`, `voltage_mode`, `ocv_table` (`NULL` → all-zero default, see above) |
| `stc3115_data_t` | `voltage_mv`, `current_ua`, `avg_current_ua`, `soc_permille` (0–1000), `temperature_c`, `ocv_mv`, `counter`, `alarm_soc`, `alarm_voltage`, `battery_fail`, `por_detect`, `charge_status`, `voltage_trend` |
| `stc3115_charge_status_t` | `STC3115_NOT_CHARGING`, `STC3115_CHARGING`, `STC3115_FULLY_CHARGED`, `STC3115_UNKNOWN` (first ~4 samples after boot, before the voltage-trend window fills) |
| `stc3115_init_status_t` | `STC3115_INIT_NEW`, `STC3115_INIT_RESTORED`, `STC3115_INIT_FAILED` |
