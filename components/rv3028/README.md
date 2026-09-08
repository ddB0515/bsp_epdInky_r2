# rv3028 — Micro Crystal RV-3028-C7 RTC driver

I2C driver for the **RV-3028-C7**, a low-power SMT real-time clock. Used on
the epdInky board as the battery-backed source of wall-clock time across
sleep/reset cycles, ahead of any network time sync.

- BCD-native time get/set (`rv3028_time_t`) and a 32-bit Unix-time register
  pair, so callers don't need `time.h` for the common seed/persist path
- Alarm on minute/hour + weekday-or-date, with per-field enable/disable and
  an interrupt flag/enable pair
- 12-bit countdown timer, four clock divisors, single-shot or repeat, with
  its own interrupt flag/enable pair
- Timestamp capture register (last recorded event, with a running count)
- Backup switchover mode and trickle-charge configuration for the backup
  battery/capacitor, written through the chip's EEPROM mirror registers
- CLKOUT frequency output, also EEPROM-mirrored
- User EEPROM byte read/write (0x00–0x2A) and a raw register read/write
  escape hatch for anything not wrapped

Requires **ESP-IDF ≥ 5.5** (`idf_component.yml`) and an I2C master bus
(`esp_driver_i2c`). Chip-agnostic beyond that; used here on **ESP32-P4**.

---

## Dependencies

| Component | Why |
|---|---|
| `esp_driver_i2c` | `i2c_master_bus_handle_t` / `i2c_master_dev_handle_t` transactions |
| `esp_driver_gpio` | Declared in `CMakeLists.txt` `REQUIRES`, but no GPIO API is called anywhere in `rv3028.c` — likely a leftover; not needed to use this component |

---

## Layout

```
rv3028/
├── include/
│   └── rv3028.h    register map, bit masks, types, full API
└── rv3028.c         implementation
```

---

## Usage

The pattern below mirrors how the board's own time layer
(`examples/idf_epd_ha_firmware/main/ha_time.c`,
`examples/trmnl-firmware/main/trmnl_time.c`) uses this driver: read the RTC
at boot before the network exists, then write SNTP's result back to it.

```c
#include "driver/i2c_master.h"
#include "rv3028.h"

// i2c_bus from i2c_new_master_bus(), or from the board's own I2C init.
rv3028_handle_t rtc;
ESP_ERROR_CHECK(rv3028_init(i2c_bus, RV3028_I2C_ADDRESS, &rtc));

// --- At boot, before the network is up: seed the system clock from the RTC ---
// PLAUSIBLE_AFTER guards against a fresh/depleted RTC reading back near the
// epoch (see ha_time.c / trmnl_time.c, which both use 1700000000).
uint32_t unix_time = 0;
esp_err_t err = rv3028_get_unix_time(rtc, &unix_time);
if (err == ESP_OK && (time_t)unix_time > PLAUSIBLE_AFTER) {
    struct timeval tv = { .tv_sec = (time_t)unix_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

// --- After a successful SNTP sync: write the corrected time back ---
time_t now = time(NULL);
esp_err_t wr = rv3028_set_unix_time(rtc, (uint32_t)now);
```

On the epdInky BSP, `bsp_rv3028_init()` (`components/epdinky_p4_board`) wraps
`rv3028_init()` with a presence probe and optional trickle-charge setup, and
hands back a `rv3028_handle_t` in `bsp_epdinky_handles_t::rv3028`. Both
firmware examples pass that handle straight to their own time layer:

```c
s_board_cfg.enable.use_rv3028 = true;
// ... bsp_epdinky_init_with_config(&s_board_cfg, &s_board) ...
ha_time_init(s_board.rv3028);      // or trmnl_time_init(s_board.rv3028)
```

`rv3028_init()` accepts a NULL result only via `ret_handle` validation — it
does **not** fail if the chip itself doesn't answer on the bus (see
[Gotchas](#gotchas) below). The BSP treats the RTC as an optional part of the
schematic and probes for it separately before calling `rv3028_init()`,
downgrading a non-response to a warning and handing back a NULL handle
instead. Application code should do the same if the RTC may be unpopulated —
every function in this driver takes the handle by value and returns
`ESP_ERR_INVALID_ARG` if it's NULL, so a `NULL` check before use (as
`ha_time_init()` / `trmnl_time_init()` / `EpdInkyBoard::readTime()` all do) is
the expected pattern rather than relying on a call failing loudly.

### Reading/setting the calendar time directly

`rv3028_time_t` fields map straight onto the chip's BCD registers with no
epoch offset — `year` is the full four-digit year (e.g. `2025`, stored on the
chip as `year - 2000`), `month` is `1-12`, `weekday` is `0-6` (0 = Sunday, the
chip does not itself define which day is 0 — it's whatever convention the
writer used):

```c
rv3028_time_t t;
ESP_ERROR_CHECK(rv3028_get_time(rtc, &t));
ESP_LOGI(TAG, "%04u-%02u-%02u %02u:%02u:%02u",
         t.year, t.month, t.date, t.hours, t.minutes, t.seconds);
```

`rv3028_set_time()` validates every field's range (`seconds/minutes < 60`,
`hours < 24`, `weekday < 7`, `date` 1–31, `month` 1–12, `year` 2000–2099) and
returns `ESP_ERR_INVALID_ARG` if any is out of range — it does not clamp or
wrap.

---

## Gotchas

**`rv3028_init()` succeeds even if the chip isn't on the bus.** It always
calls `i2c_master_bus_add_device()` (which only registers a software device
descriptor — I2C traffic doesn't happen yet) and then reads the chip ID as a
diagnostic. If that read fails, it logs a warning and still returns
`ESP_OK` with a valid handle. Presence must be confirmed with an explicit
transaction (`rv3028_get_id()`, or a bus probe as the BSP does) if your code
needs to know the part actually responded, rather than checking
`rv3028_init()`'s return value.

**24-hour mode is assumed, not enforced.** The driver never reads or writes
`RV3028_CTRL2_12_24` (Control 2, bit 1). `rv3028_get_time()`/`rv3028_set_time()`
mask the hours byte with `0x3F`, which is only correct in 24-hour mode — if
the chip were ever left in 12-hour mode (by other software, or a part that
didn't power up in its default state), hours would decode incorrectly with no
error indication. Nothing in this component sets the mode explicitly.

**Backup switchover mode is never configured by the BSP.** `bsp_rv3028_init()`
only calls `rv3028_set_trickle_charge()`, and only if
`opts.rv3028_trickle_chg_en` is set (it defaults to **false** in
`bsp_epdinky_default_config()`). `rv3028_set_backup_mode()` is never called
anywhere in this repo, so the backup switchover mode is whatever the chip's
EEPROM/RAM mirror already holds (`RV3028_BSM_DISABLED` is the reset default
per the bit layout, but this driver does not verify or set it). If the
board's backup cell/capacitor needs switchover behaviour, call
`rv3028_set_backup_mode()` explicitly.

**Settings routed through the EEPROM mirror are slow and can block.**
`rv3028_set_backup_mode()`, `rv3028_set_trickle_charge()`, `rv3028_set_clkout()`,
and both `rv3028_eeprom_read()`/`rv3028_eeprom_write()` all go through a
shared internal EEPROM-command path that disables auto-refresh (`CTRL1_EERD`),
polls `STATUS_EEBUSY` (up to 100 ms, twice — once before and once after
issuing the command), sends the command, and only then re-enables
auto-refresh. Worst case that's on the order of 200 ms of polling per call;
don't call these from a tight loop or a latency-sensitive path.

**The alarm's "disabled" sentinel differs between write and read.**
`rv3028_set_alarm()` treats `minutes > 59`, `hours > 23`, or
`weekday_date > 31` as "disable this field" and writes the chip's AE (bit 7)
disable bit; `rv3028_get_alarm()` reports a disabled field back as `0xFF`,
not the original out-of-range value you may have written. Don't compare
`rv3028_get_alarm()`'s output against your original disable sentinel — check
for `0xFF` specifically.

**I2C address is fixed.** The RV-3028-C7 doesn't support address selection;
`rv3028_init()` takes an `address` parameter, but in practice it should
always be `RV3028_I2C_ADDRESS` (`0x52`), which is also what
`I2C_DEVICE_RV3028_ADDR` in `components/epdinky_p4_board/include/bsp/config.h`
is set to.

---

## API reference

| Function | Purpose |
|---|---|
| `rv3028_init(bus, address, *ret_handle)` | Add the device to an existing I2C master bus (400 kHz), probe the chip ID as a diagnostic (non-fatal), return a handle |
| `rv3028_deinit(handle)` | Remove the I2C device and free the handle |
| `rv3028_reset(handle)` | Set `CTRL2_RESET`, wait 10 ms |
| `rv3028_get_id(handle, *id)` | Read the chip ID register (`0x28`) |
| `rv3028_get_time(handle, *time)` / `rv3028_set_time(handle, *time)` | BCD calendar time via `rv3028_time_t` (seconds/minutes/hours/weekday/date/month/year); set validates ranges and rejects out-of-range fields |
| `rv3028_get_unix_time(handle, *unix_time)` / `rv3028_set_unix_time(handle, unix_time)` | 32-bit Unix time, registers `0x1B`–`0x1E`, little-endian on the wire |
| `rv3028_set_alarm(handle, *alarm)` / `rv3028_get_alarm(handle, *alarm)` | Minute/hour/weekday-or-date alarm via `rv3028_alarm_t`; out-of-range input disables a field on write, disabled fields read back as `0xFF` |
| `rv3028_enable_alarm_interrupt(handle, enable)` | `CTRL2_AIE` |
| `rv3028_get_alarm_flag(handle, *flag)` / `rv3028_clear_alarm_flag(handle)` | Status register `AF` bit |
| `rv3028_set_timer(handle, value, freq, repeat)` | 12-bit countdown value (0–4095), `rv3028_timer_freq_t` divisor, single-shot or repeat (`CTRL1_TRPT`); disables the timer first |
| `rv3028_enable_timer(handle, enable)` | `CTRL1_TE` |
| `rv3028_enable_timer_interrupt(handle, enable)` | `CTRL2_TIE` |
| `rv3028_get_timer_flag(handle, *flag)` / `rv3028_clear_timer_flag(handle)` | Status register `TF` bit |
| `rv3028_enable_timestamp(handle, enable)` | `CTRL2_TSE` |
| `rv3028_get_timestamp(handle, *ts)` | Last timestamp event via `rv3028_timestamp_t` (count + BCD time) |
| `rv3028_reset_timestamp(handle)` | `EVENT_CONTROL` register `TSR` bit |
| `rv3028_set_backup_mode(handle, mode)` | `rv3028_backup_mode_t` (disabled/direct/level ~2.0 V/level ~1.4 V) written to the EEPROM Backup register mirror and committed to EEPROM |
| `rv3028_set_trickle_charge(handle, enable, resistor)` | `rv3028_trickle_resistor_t` (3 k/5 k/9 k/15 kΩ), same EEPROM-mirror path |
| `rv3028_get_backup_flag(handle, *flag)` / `rv3028_clear_backup_flag(handle)` | Status register `BSF` bit (backup switchover occurred) |
| `rv3028_set_clkout(handle, enable, freq)` | `rv3028_clkout_freq_t` (32.768 kHz down to 1 Hz, or static low), EEPROM-mirror path |
| `rv3028_eeprom_read(handle, addr, *data)` / `rv3028_eeprom_write(handle, addr, data)` | User EEPROM byte access, `addr` `0x00`–`0x2A` |
| `rv3028_eeprom_wait_busy(handle, timeout_ms)` | Poll `STATUS_EEBUSY` until clear or timeout (used internally by every EEPROM-mirror call) |
| `rv3028_get_status(handle, *status)` | Raw status register (`0x0E`) |
| `rv3028_get_por_flag(handle, *flag)` / `rv3028_clear_por_flag(handle)` | Status register `PORF` bit (power-on reset occurred — a fresh/depleted backup supply reads as this on next power-up) |
| `rv3028_clear_all_flags(handle)` | Writes `0x00` to the status register, clearing every flag at once |
| `rv3028_read_reg(handle, reg, *data, len)` / `rv3028_write_reg(handle, reg, *data, len)` | Raw multi-byte register access for anything not wrapped above |

All functions return `esp_err_t`; every public function validates its handle
and out-parameters and returns `ESP_ERR_INVALID_ARG` on a NULL/invalid one.
