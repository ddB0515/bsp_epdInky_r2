# esp_lcd_jd9168 — JD9168 MIPI-DSI panel driver

`esp_lcd` panel driver for the **JD9168**, a MIPI-DSI video-mode LCD driver IC,
wired up here for the **D320C2403V-MIPI** module (3.2", 1024×768 IPS). It
plugs into ESP-IDF's generic `esp_lcd_panel_t` interface the same way any
other `esp_lcd` vendor driver does: create an MIPI-DSI bus and a DBI (command)
IO on top of it, hand both to `esp_lcd_new_panel_jd9168()`, and you get back a
normal `esp_lcd_panel_handle_t` — `esp_lcd_panel_init()`,
`esp_lcd_panel_disp_on_off()`, `esp_lcd_panel_mirror()`, etc. all work as
usual.

- Wraps `esp_lcd_new_panel_dpi()` — the actual video-out (DPI) side is
  ESP-IDF's own MIPI-DSI panel; this driver only adds the JD9168 command
  sequence and DCS glue on top
- Ships a validated default init sequence (`vendor_specific_init_default`),
  overridable via `jd9168_vendor_config_t::init_cmds`
- Config macros for this exact module's DSI bus, DBI IO and DPI video timing
  (`JD9168_PANEL_BUS_DSI_2CH_CONFIG`, `JD9168_PANEL_IO_DBI_CONFIG`,
  `JD9168_1024_768_PANEL_60HZ_DPI_CONFIG`)
- `mirror()` implemented via `LCD_CMD_MADCTL`; `swap_xy()` and `set_gap()` are
  **not supported by this panel** and return `ESP_ERR_NOT_SUPPORTED`

Requires **ESP-IDF ≥ 5.5** with `esp_lcd`'s MIPI-DSI support (`esp_lcd_mipi_dsi.h`)
and a chip with a MIPI-DSI peripheral (ESP32-P4). Developed and verified on
**ESP32-P4** (epdInky board + D320C2403V-MIPI adapter).

---

## Specifications — D320C2403V-MIPI

| Field | Value |
|---|---|
| Model name | D320C2403V-MIPI |
| Screen size | 3.2" |
| Resolution | 1024 × 768 |
| Outline size (excl. FPC) | 70.89 (W) × 57.19 (H) × 3.45 (T) mm |
| Active area | 64.51 (W) × 48.384 (H) mm |
| LCD interface | MIPI-DSI, video mode |
| LCD driver IC | JD9168 |
| Touch | Capacitive touch screen (CTP) |
| Touch interface | I2C |
| Touch driver IC | GT967 |
| Brightness | 600 cd/m² |
| Display type | IPS |
| Operating temperature | −20 °C … +70 °C |

Full datasheet: [D320C2403V1(MIPI).pdf](https://github.com/ddB0515/display-panels-datasheets/blob/main/datasheets/D/D320C2403V1(MIPI).pdf)

**Touch note:** there is no dedicated GT967 driver in this repo. The GT967 is
a Goodix GT9xx part that speaks the same register protocol as the GT911, so
the registry's `espressif/esp_lcd_touch_gt911` driver handles it as-is — see
[Touch](#touch-gt967-via-esp_lcd_touch_gt911) below.

---

## Dependencies

| Component | Why |
|---|---|
| `esp_lcd` | `esp_lcd_panel_t`, DBI panel IO, MIPI-DSI bus/DPI panel |
| `driver` | `driver/gpio.h` for the optional hardware reset line |

Not linked by this component, but needed to bring the panel up on hardware
that gates power/reset/backlight through an I2C expander (like the epdInky
D320C2403V-MIPI adapter — see [Bring-up](#bring-up-notes) below):

| Component | Why |
|---|---|
| `tca6408` | GPIO expander driving panel power, reset and backlight enable |
| `sgm37604a` | I2C backlight/LED driver on the module |
| `espressif/esp_lcd_touch_gt911` (registry) | Touch — see [Touch](#touch-gt967-via-esp_lcd_touch_gt911) |

---

## Layout

```
esp_lcd_jd9168/
├── include/
│   └── esp_lcd_jd9168.h   public API: create fn, vendor config, bus/IO/DPI macros
└── esp_lcd_jd9168.c       DCS init sequence, esp_lcd_panel_t op overrides
```

---

## Usage

```c
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9168.h"

// 1. MIPI-DSI bus: 2 data lanes at 800 Mbps (see the DPI clock note below —
//    do not pair this with a different lane rate without re-checking it).
esp_lcd_dsi_bus_config_t bus_config = JD9168_PANEL_BUS_DSI_2CH_CONFIG();
esp_lcd_dsi_bus_handle_t dsi_bus;
ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

// 2. DBI IO: the command channel used for the JD9168's DCS init sequence.
esp_lcd_dbi_io_config_t dbi_config = JD9168_PANEL_IO_DBI_CONFIG();
esp_lcd_panel_io_handle_t io;
ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &io));

// 3. DPI video timing for this module, 1024x768. RGB888 is required — see below.
esp_lcd_dpi_panel_config_t dpi_config =
    JD9168_1024_768_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB888);

jd9168_vendor_config_t vendor_config = {
    .mipi_config = {
        .dsi_bus    = dsi_bus,
        .dpi_config = &dpi_config,
    },
    // .init_cmds = NULL uses the driver's own validated default sequence
};

esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = GPIO_NUM_NC,           // -1 if reset is wired through an
                                              // expander/other component instead
    .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 24,                    // must match LCD_COLOR_FMT_RGB888 above
    .vendor_config  = &vendor_config,
};

esp_lcd_panel_handle_t panel;
ESP_ERROR_CHECK(esp_lcd_new_panel_jd9168(io, &panel_config, &panel));

ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));   // or your own reset sequence — see below
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
```

Once `esp_lcd_panel_init()` returns, the panel is in DPI video mode: whatever
the DSI controller's frame buffer holds is scanned out continuously. There is
no `draw_bitmap()` "push a rectangle" step like an SPI/QSPI panel — get the
buffer `esp_lcd_new_panel_dpi()` allocated (`num_fbs = 1` in the macro above,
i.e. no double-buffering) with `esp_lcd_dpi_panel_get_frame_buffer()`, write
pixels into it directly, and they appear on the next scan-out:

```c
void *fb = NULL;
ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb));
```

**The frame buffer lives in PSRAM and is read out by DMA, not by the CPU.**
A plain CPU write only lands in cache; without an explicit write-back the
panel shows whatever pixels happened to already be evicted — streaks of
stale content rather than what you just wrote. `esp_lcd_panel_draw_bitmap()`
does this internally, which is why panels driven through that call don't
need it, but writing straight into the DPI frame buffer does:

```c
esp_cache_msync(addr, size,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
```

---

## Bring-up notes

**RGB888 only.** `LCD_COLOR_FMT_RGB565` was tried on this exact panel and
produced a blank screen — not even the DSI controller's own colour-bar test
pattern reached the glass. The JD9168's init sequence evidently expects
24-bit pixels; `bits_per_pixel` in `esp_lcd_panel_dev_config_t` must be `24`
to match.

**The DPI clock and lane rate are a matched pair, not independent knobs.**
`JD9168_1024_768_PANEL_60HZ_DPI_CONFIG` pairs a 48 MHz pixel clock with the
2-lane, 800 Mbps bus from `JD9168_PANEL_BUS_DSI_2CH_CONFIG()` — that is the
combination both examples in this repo actually run with. The vendor's own
reference header ships 50 MHz / 900 Mbps instead, and that combination was
tried on this hardware: the panel never locks onto video (the DSI bridge
stops scanning the frame buffer out, so no VSYNC is produced), while the
host's own test pattern keeps displaying regardless and hides the problem —
so a blank screen with a *working* test pattern points here, not at the frame
buffer. Do not "correct" the timings back to the vendor macro's values.

> **Known inconsistency worth checking before you rely on it:** the macro's
> own comment in `esp_lcd_jd9168.h` says the pixel clock is "45 MHz", but
> the macro itself sets `.dpi_clock_freq_mhz = 48` — the comment is stale. A
> separate `BSP_DSI_LCD_DPI_CLK_MHZ` constant in `epdinky_p4_board`'s
> `bsp/config.h` is also set to `45` but isn't actually wired into either
> example's `dpi_config`; it looks like a leftover from before the macro was
> retuned to 48. Treat the macro's `48` as the currently-working value if
> you're touching this — and update both the macro's comment and the BSP
> constant together so they stop disagreeing with the code.

**`swap_xy()` and `set_gap()` are not supported** and return
`ESP_ERR_NOT_SUPPORTED` — handle orientation via `rgb_ele_order` /
`esp_lcd_panel_mirror()` or by rotating in software instead.

**Software vs. hardware reset.** If `reset_gpio_num` is a valid GPIO, the
driver drives it directly (active level from
`panel_dev_config->flags.reset_active_high`). If it's `-1`/`GPIO_NUM_NC`,
`esp_lcd_panel_reset()` instead sends a DCS software-reset command over the
DBI IO. On hardware where panel reset is wired through a GPIO expander rather
than an ESP32 GPIO (see below), create the panel with `reset_gpio_num = -1`
and do the hardware reset yourself through the expander *before* calling
`esp_lcd_panel_init()` — then skip `esp_lcd_panel_reset()` entirely, since a
DCS software reset at that point would just put the JD9168 back to its
power-on defaults right before the init sequence runs anyway.

**Reading the panel ID.** `panel_jd9168_init()` reads three ID bytes via DCS
command `0x04` before sending anything else, purely as a diagnostic
(`esp_lcd_panel_io_rx_param`) — if this fails, the DBI IO / DSI bus isn't
talking to the panel at all, which is worth checking before chasing an init
sequence problem.

---

## Bring-up on the epdInky D320C2403V-MIPI adapter

On the epdInky ESP32-P4 board's DSI adapter, the panel, its backlight and its
reset line are all gated by a second `tca6408` I/O expander at I2C address
`0x20` (distinct from the mainboard's own expander at `0x21`):

| Expander pin | Drives | Notes |
|---|---|---|
| P0 | AP2281 load switch → panel `LCD_VDD` | panel is completely unpowered until this is high |
| P1 | JD9168 reset (active low) | a register write, not an ESP32 GPIO — see above |
| P2 | SGM37604A `HWEN` | backlight IC doesn't ACK on I2C at all until this is high |

**Order matters**, and getting it wrong looks like a disconnected display
rather than a sequencing bug: power the panel, wait for rails to settle,
release reset, *then* enable and configure the backlight controller. A bus
scan taken before P0/P2 are driven shows neither the backlight's `0x36` nor
the touch controller's `0x5D` on the bus, which is easy to mistake for a
missing adapter board.

See `examples/idf_ha_firmware/main/ha_panel_jd9168.c` for the full working
sequence (expander init → power → reset → backlight → DSI bus/IO → panel
create → init → display on), and
`examples/idf_dsi_camera_preview/main/app_display.c` for the original
bring-up this was adapted from, including the RGB565-vs-RGB888 and
DPI-clock investigation notes in more detail.

---

## Touch (GT967 via `esp_lcd_touch_gt911`)

The GT967 is a Goodix GT9xx part that speaks the same register protocol as
the GT911, so the stock `espressif/esp_lcd_touch_gt911` component (pulled
from the component registry) drives it without modification — there is no
GT967-specific component in this repo. Add it to `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_touch_gt911: "^1.2.0"
```

Usage is the standard `esp_lcd_touch` pattern — an I2C panel IO plus
`esp_lcd_touch_new_i2c_gt911()`:

```c
#include "esp_lcd_touch_gt911.h"

esp_lcd_panel_io_handle_t touch_io;
esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
io_cfg.dev_addr = 0x5D;  // GT967 address, selected by its INT pin at reset
ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &touch_io));

const esp_lcd_touch_config_t cfg = {
    .x_max        = 1024,
    .y_max        = 768,
    .rst_gpio_num = /* touch reset GPIO, or GPIO_NUM_NC */,
    .int_gpio_num = /* touch interrupt GPIO, or GPIO_NUM_NC */,
    .levels = { .reset = 0, .interrupt = 0 },
    .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
};

esp_lcd_touch_handle_t touch;
ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(touch_io, &cfg, &touch));

// Poll (e.g. from an LVGL indev read callback):
uint16_t x, y; uint8_t count;
esp_lcd_touch_read_data(touch);
bool pressed = esp_lcd_touch_get_coordinates(touch, &x, &y, NULL, &count, 1);
```

See `examples/idf_ha_firmware/main/ha_touch_gt911.c` for a complete LVGL
`indev` read-callback implementation built on this.

---

## Backlight (SGM37604A)

The D320C2403V-MIPI module's backlight is driven by an SGM37604A LED driver
IC over I2C (address `0x36`), controlled with the `sgm37604a` component in
this repo — not part of `esp_lcd_jd9168` itself, since backlight control is
independent of the panel command interface:

```c
#include "sgm37604a.h"

ESP_ERROR_CHECK(sgm37604a_init(i2c_bus, SGM37604A_CURRENT_30MA));
sgm37604a_set_brightness_percent(80);   // 0-100
```

The backlight IC will not respond on the I2C bus at all until its hardware
enable pin is driven high (see the adapter's P2 expander pin above) — probe
it only after that.

---

## API reference

| Symbol | Purpose |
|---|---|
| `esp_lcd_new_panel_jd9168(io, panel_dev_config, &panel)` | Create the panel. `panel_dev_config->vendor_config` must point at a `jd9168_vendor_config_t`. |
| `jd9168_vendor_config_t` | `init_cmds`/`init_cmds_size` (NULL = default sequence), `mipi_config.{dsi_bus,dpi_config}` |
| `jd9168_lcd_init_cmd_t` | One `{cmd, data, data_bytes, delay_ms}` entry for a custom init sequence |
| `JD9168_PANEL_BUS_DSI_2CH_CONFIG()` | `esp_lcd_dsi_bus_config_t` initialiser: 2 lanes, 800 Mbps |
| `JD9168_PANEL_IO_DBI_CONFIG()` | `esp_lcd_dbi_io_config_t` initialiser: 8-bit cmd/param |
| `JD9168_1024_768_PANEL_60HZ_DPI_CONFIG(px_format)` | `esp_lcd_dpi_panel_config_t` initialiser for this module's 1024×768 timing |

Once created, the panel is a normal `esp_lcd_panel_handle_t` — use the
generic `esp_lcd_panel_*()` API (`init`, `reset`, `disp_on_off`, `mirror`,
`invert_color`, `del`) rather than any JD9168-specific functions for
everything after creation.
