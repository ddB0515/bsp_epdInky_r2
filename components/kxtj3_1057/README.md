# kxtj3_1057 — Kionix KXTJ3-1057 accelerometer driver

Minimal driver for the **Kionix KXTJ3-1057**, a 3-axis I2C accelerometer,
built on ESP-IDF's `driver/i2c_master.h` bus/device API.

- Device add/probe/init on an existing `i2c_master_bus_handle_t` — the bus
  itself is owned by the caller, not this component
- `±2g` / `±4g` / `±8g` range selection, selectable output data rate, and a
  high-resolution (14-bit) / low-resolution (12-bit) mode switch
- Raw 16-bit signed axis reads, already shifted down to the active
  resolution's valid bit width
- Raw-counts → milli-g conversion using the range/resolution that was last
  passed to `kxtj3_configure()`

Requires **ESP-IDF ≥ 5.5** and an I2C master bus (any ESP-IDF target
supported by `esp_driver_i2c`). Used on the epdInky **ESP32-P4** board.

---

## Dependencies

| Component | Why |
|---|---|
| `esp_driver_i2c` | `i2c_master_bus_handle_t` / `i2c_master_dev_handle_t` transactions |
| `esp_driver_gpio` | Listed in `CMakeLists.txt` `REQUIRES`; not used directly by any function in this driver (see note below) |

`idf_component.yml` pins `idf: ">=5.5"` and declares no registry dependencies.

---

## Layout

```
kxtj3_1057/
├── include/
│   └── kxtj3_1057.h    public API: handle type, registers/bitmasks, range enum
└── kxtj3_1057.c         I2C transactions, ODR/range mapping, raw-to-mg conversion
```

---

## Usage

```c
#include "kxtj3_1057.h"

i2c_master_bus_handle_t bus = /* existing I2C master bus */;

kxtj3_handle_t accel = NULL;
ESP_ERROR_CHECK(kxtj3_init(bus, KXTJ3_I2C_ADDR, &accel));

// kxtj3_init() already leaves the device sampling at +/-2g, 100 Hz, high-res,
// data-ready interrupt disabled (see "Defaults after kxtj3_init" below).
// Call kxtj3_configure() again only to change those settings, e.g.:
ESP_ERROR_CHECK(kxtj3_configure(accel, KXTJ3_RANGE_4G, 50,
                                 /*high_res=*/true, /*data_ready_int=*/false));

int16_t x, y, z;
float x_mg, y_mg, z_mg;
if (kxtj3_read_raw(accel, &x, &y, &z) == ESP_OK) {
    kxtj3_raw_to_mg(accel, x, y, z, &x_mg, &y_mg, &z_mg);
    ESP_LOGI("app", "Accel: X=%.0f mg Y=%.0f mg Z=%.0f mg", x_mg, y_mg, z_mg);
}

kxtj3_delete(accel, bus);   // removes the I2C device, does not touch the bus
```

`kxtj3_init()` calls `i2c_master_probe()` before adding the device, so it
returns the probe's own error (not a generic transaction failure) if nothing
ACKs at `address`. `KXTJ3_I2C_ADDR` (`0x0F`) is only one of the two possible
addresses set by the part's `SA0` strap — the epdInky BSP (`epdinky_p4_board`)
probes `0x0F` and falls back to `0x0E` since `SA0` is a board jumper, not a
fixed strap. If you don't know how `SA0` is wired, probe both.

`kxtj3_read_raw()` returns values already right-shifted to the resolution
configured by the most recent `kxtj3_configure()` call (or by `kxtj3_init()`'s
own defaults) — 2 bits for high-res/14-bit, 4 bits for low-res/12-bit.
`kxtj3_raw_to_mg()` reads the handle's stored range and resolution to pick the
matching counts-per-g scale factor, so always call it with counts obtained
*after* the configuration you want converted — it does not take range/
resolution as explicit arguments.

---

## Defaults after `kxtj3_init()`

`kxtj3_init()` unconditionally calls `kxtj3_configure(dev, KXTJ3_RANGE_2G, 100,
/*high_res=*/true, /*data_ready_int=*/false)` before returning, so a freshly
initialized device is already sampling at ±2g, ~100 Hz, 14-bit resolution,
with the data-ready interrupt (`DRDYE`) disabled. A subsequent
`kxtj3_configure()` call is only needed to change these.

`kxtj3_configure()` itself always: writes `CTRL_REG1 = 0x00` (standby, `PC1`
cleared) and waits 10 ms, writes `DATA_CTRL`, then writes a new `CTRL_REG1`
with `PC1` set — i.e. every call is a full stop/reconfigure/start cycle, not
an incremental update.

---

## Bring-up notes

**WHO_AM_I is logged, not enforced.** `kxtj3_init()` reads `WHO_AM_I`
(register `0x0F`) and logs the value against the expected `0x35`
(`KXTJ3_WHO_AM_I_VALUE`), but does not fail initialization if they differ —
a mismatch only shows up as an `ESP_LOGI` line. The header's own comment
notes this expected value is "commonly 0x35" for the KXTJ3-1057, i.e. treat
it as the driver author's best-known value rather than a value pulled from a
datasheet copy in this repo.

**ODR is quantized to eight fixed steps.** `kxtj3_configure()`'s `odr_hz`
parameter is not written to the device as-is; `kxtj3_odr_to_data_ctrl()` maps
it down to the closest *lower or equal* supported rate: 800, 400, 200, 100,
50, 25, or 12.5 Hz (anything below 25 Hz clamps to 12.5 Hz, the `DATA_CTRL`
`0x00` case). Passing, say, `60` silently yields 50 Hz, not an error.

**`data_ready_int` only sets `DRDYE`.** `kxtj3_configure()`'s
`data_ready_int` argument just OR's `KXTJ3_CTRL1_DRDYE` (`0x20`) into
`CTRL_REG1` — this driver has no interrupt-pin/GPIO handling and no
`INT_CTRL_REG1`/`INT_SOURCE` register writes, so enabling it only makes the
device assert its own `INT` pin; wiring and servicing that interrupt is
entirely up to the caller.

**`kxtj3_delete()` does not touch the bus.** It writes `CTRL_REG1 = 0x00`
best-effort (return value discarded) to put the device in standby, then
removes only the I2C *device* (`i2c_master_bus_rm_device`) and frees the
handle. The `bus_handle` parameter is accepted but explicitly unused —
per the source comment, "bus is owned by app; do not delete it here".

**`kxtj3_config_t` is unused.** The header defines a `kxtj3_config_t` struct
containing a single `_reserved` byte, annotated "kept for compatibility if
referenced elsewhere" — no function in this driver takes or returns it.

---

## API reference

| Symbol | Purpose |
|---|---|
| `kxtj3_init(bus_handle, address, &handle)` | Probes `address` on `bus_handle`, adds the I2C device (400 kHz, 7-bit address), reads `WHO_AM_I` (logged only), then configures ±2g/100 Hz/high-res/no interrupt as described above |
| `kxtj3_configure(handle, range, odr_hz, high_res, data_ready_int)` | Standby → set `DATA_CTRL` (quantized ODR) → set `CTRL_REG1` (range, `RES`, `DRDYE`, `PC1`) and resume measurement |
| `kxtj3_delete(handle, bus_handle)` | Best-effort standby, removes the I2C device, frees `handle`. `NULL` handle is a no-op that returns `ESP_OK` |
| `kxtj3_read_raw(handle, &x, &y, &z)` | Reads 6 bytes from `XOUT_L` (`0x06`) as little-endian pairs, right-shifted 2 (high-res) or 4 (low-res) bits |
| `kxtj3_raw_to_mg(handle, x, y, z, &x_mg, &y_mg, &z_mg)` | Converts raw counts to milli-g using the handle's stored range/resolution (16384/8192/4096 counts/g at 14-bit; 4096/2048/1024 counts/g at 12-bit, for 2g/4g/8g respectively) |
| `kxtj3_handle_t` | Opaque pointer to the driver's internal device state |
| `kxtj3_range_t` | `KXTJ3_RANGE_2G` (2), `KXTJ3_RANGE_4G` (4), `KXTJ3_RANGE_8G` (8) |
| `KXTJ3_I2C_ADDR` | `0x0F` — one of two possible addresses; the other depends on the `SA0` strap (see Usage) |
| `KXTJ3_REG_XOUT_L`, `KXTJ3_REG_WHO_AM_I`, `KXTJ3_REG_CTRL_REG1`, `KXTJ3_REG_CTRL_REG2`, `KXTJ3_REG_DATA_CTRL` | Register addresses `0x06`, `0x0F`, `0x1B`, `0x1D`, `0x21`. `CTRL_REG2` is defined but never read or written by this driver |
| `KXTJ3_CTRL1_PC1`, `_RES`, `_DRDYE`, `_GSEL_2G`, `_GSEL_4G`, `_GSEL_8G` | `CTRL_REG1` bitmasks: `0x80`, `0x40`, `0x20`, `0x00`, `0x08`, `0x10` |
| `KXTJ3_WHO_AM_I_VALUE` | `0x35` — expected `WHO_AM_I` reply, logged but not enforced |
