# epd — raw E Ink panel driver

Drives **raw** E Ink panels — bare glass with source and gate driver ICs and no
timing controller — from an ESP32 chip with the LCD i80 peripheral. The chip
*is* the timing controller: every clock edge that reaches the glass is generated
by this component.

- 16-level greyscale (GC16) from temporal phases plus an 8×8 spatial dither
- INIT (ghost-clearing) and DU (two-level) waveforms
- A catalogue of thirteen validated panels, 300×1440 up to 2760×2070, 8- and 16-bit
- 4bpp framebuffer with drawing, text, and image scale/rotate
- Temperature-compensated frame counts from the PMIC thermistor
- Async row DMA overlapped with row construction

Requires **ESP-IDF ≥ 6.0** and a chip with the LCD i80 peripheral
(ESP32-S2/S3/P4). Developed and verified on **ESP32-P4**.

---

## Dependencies

| Component | Why |
|---|---|
| `esp_lcd` | i80 bus and panel IO |
| `esp_driver_gpio` | bit-banged gate and latch lines |
| `esp_timer` | row pacing and statistics |
| `tps65185` | PMIC: rails, VCOM, thermistor |

The PMIC is a hard dependency — the driver sets VCOM, programs the rail power-up
order from the panel definition, and reads the thermistor for temperature
compensation.

---

## Layout

```
epd/
├── include/
│   ├── epd_board.h       board wiring (application supplies this)
│   ├── epd_display.h     panel creation entry point
│   ├── epd_panel.h       generic panel lifecycle and refresh API
│   ├── epd_panel_def.h   epd_panel_def_t — the panel contract
│   ├── epd_panels.h      the panel catalogue
│   ├── epd_i80_bus.h     low-level bus (rarely needed directly)
│   ├── epd_fb.h          framebuffer, drawing, text, image blit
│   └── gfxfont.h         Adafruit-GFX font format
└── src/
```

**Board wiring is not part of a panel definition.** Swap the panel on one board
and the pins are unchanged; move the panel to another board and they all differ.
Panels live in `epd_panels.c`, pins live in your application or BSP.

---

## Usage

```c
#include "epd_display.h"
#include "epd_panels.h"
#include "epd_fb.h"

static const epd_board_config_t board = {
    .data = { GPIO_NUM_2,  GPIO_NUM_3,  GPIO_NUM_4,  GPIO_NUM_5,
              GPIO_NUM_6,  GPIO_NUM_7,  GPIO_NUM_8,  GPIO_NUM_9,
              GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13,
              GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17 },
    .cl = GPIO_NUM_50, .le = GPIO_NUM_48, .oe  = GPIO_NUM_47,
    .sph = GPIO_NUM_46, .spv = GPIO_NUM_45, .ckv = GPIO_NUM_51,
    .gmod = GPIO_NUM_52,
    .dc_dummy = GPIO_NUM_24,      // any genuinely free GPIO — see below
    .oe_active_high = true,
};

epd_panel_handle_t panel;
ESP_ERROR_CHECK(epd_display_panel_create(&epd_panel_eink_ed103tc2,
                                         &board, pmic, &panel));

epd_fb_t fb;
ESP_ERROR_CHECK(epd_fb_create(&fb, epd_panel_eink_ed103tc2.width,
                                   epd_panel_eink_ed103tc2.height));
epd_fb_fill(&fb, 0xF);                              // 0xF = white
epd_fb_draw_string(&fb, 20, 40, "Hello", 2, 0x0, 0xF);  // scale, fg, bg

ESP_ERROR_CHECK(epd_panel_power_on(panel));
epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_INIT);   // clear ghosting
epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_GC16);   // draw
epd_panel_power_off(panel);   // image persists — the panel is bistable
```

`INIT` before `GC16` is not optional. GC16 phases can only darken, so the
greyscale model assumes a white baseline.

`power_off` leaves the PMIC in **STANDBY**, not sleep. Dropping its WAKEUP pin
powers down the I2C interface entirely, which is a system-level decision — call
`tps65185_sleep()` yourself if nothing else needs the PMIC.

---

## Images

`epd_fb_blit()` takes a 4bpp source in the framebuffer's own packing.
`epd_fb_blit_1bpp()` takes one bit per pixel, MSB-first, which is what BMP files
and Adafruit GFX bitmaps produce:

```c
epd_bitmap1_t bmp = {
    .bits      = payload,     // MSB-first: bit 7 is the leftmost pixel
    .width     = 1872,
    .height    = 1404,
    .stride    = 0,           // 0 = (width + 7) / 8; BMP wants 4-byte rows
    .bottom_up = false,       // true for an ordinary BMP: last row stored first
    .invert    = false,       // false: a set bit is white
};

epd_fb_blit_1bpp(&fb, 0, 0, &bmp);
epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_DU);   // ~0.3 s
```

`stride` and `bottom_up` exist so a downloaded BMP can be blitted straight out
of its receive buffer, with no intermediate copy or row-flipping pass.

The result contains only 0x0 and 0xF, so **DU is the right waveform** — it is
several times faster than GC16 and there are no intermediate greys for GC16 to
render.

`epd_fb_blit_1bpp_centered()` and `epd_fb_blit_1bpp_fit[_rot]()` mirror their
4bpp counterparts. Note that only 1:1 and integer upscaling stay purely black
and white: downscaling averages over the source box, so edges come out grey.
Prefer rendering at the panel's own resolution.

Not every image service is bilevel — a greyscale source belongs in the 4bpp
path, decoded straight into the framebuffer's packing and refreshed with
`EPD_WAVEFORM_GC16` after an INIT pass. `examples/trmnl-firmware` does exactly
that: trmnl.app sends 4-bit PNG at the panel's native resolution, so the decoded
rows *are* framebuffer rows and the 1-bpp blit above is only used for the
locally drawn status screens.

---

## Using with the epdinky_p4_board BSP

The board config maps directly onto the BSP's pin macros:

```c
#include "bsp/config.h"

static const epd_board_config_t board = {
    .data = BSP_EPD_DATA_PINS_DEFAULT,
    .cl   = BSP_EPD_PIN_XCL,
    .le   = BSP_EPD_PIN_XLE,
    .oe   = BSP_EPD_PIN_XOE,
    .sph  = BSP_EPD_PIN_XSTL,
    .spv  = BSP_EPD_PIN_SPV,
    .ckv  = BSP_EPD_PIN_CKV,
    .gmod = BSP_EPD_PIN_MODE,
    .dc_dummy = GPIO_NUM_24,
    .oe_active_high = true,
};
```

`epd_board_config_t::data` is deliberately in the panel's own D0..D15 numbering
so `BSP_EPD_DATA_PINS_DEFAULT` drops in unchanged. Get the PMIC handle from
`bsp_tps65185_init()`.

**On `dc_dummy`.** An EPD has no command/data line, but the i80 peripheral
requires a valid DC pin — IDF rejects a negative value — and drives it
continuously through the GPIO matrix. It idles high and never toggles
(`lcd_cmd_bits` is 0), but it *is* driven, so it must point at a GPIO nothing
else uses. On the epdInky board GPIO 24, 25 and 36 are unassigned in the BSP pin
map. **GPIO 38 is `BSP_TPS65185_PIN_INT`** and is not a valid choice — pointing
DC at it makes the peripheral fight the PMIC's active-low interrupt output.

---

## The data pin ordering, and why it's expressed this way

The LCD peripheral always emits the **first byte** of a transfer on its data
signals 0..7 and the second on 8..15. It has no idea which physical lines the
panel calls D0–D7. Since `epd_board_config_t::data` is in the panel's numbering,
the component applies that mapping for you.

This matters because getting it wrong on an 8-bit panel is **silent and total**:
the peripheral drives lines the panel doesn't have, the panel's real inputs stay
at `0x00` which decodes as "no drive", and the screen shows **nothing at all**,
not even the INIT flash. If a panel is completely blank, suspect `bus_width`
before anything else.

For a 16-bit panel that comes out scrambled in a way `EPD_PANEL_FLAG_MIRROR_X`
doesn't fix, try `bus16_low_byte_first`.

---

## Panel catalogue

| Panel | Size (W×H) | Bus | VCOM (mV) |
|---|---|---|---|
| `epd_panel_eink_ed103tc2` | 1872×1404 | 16 | 1200 |
| `epd_panel_eink_ed097tc2` | 1200×825 | 8 | 1620 |
| `epd_panel_eink_ed113tc1` | 2400×1034 | 16 | 1200 |
| `epd_panel_eink_ed060scp` | 800×600 | 8 | 1600 |
| `epd_panel_eink_ed052tc4` | 1280×720 | 8 | 2570 |
| `epd_panel_eink_ed115oc1` | 2760×2070 | 16 | 1340 |
| `epd_panel_eink_ed133ut2` | 2200×1650 | 16 | 1890 |
| `epd_panel_eink_ed070kh1` | 1264×1680 | 16 | 1400 |
| `epd_panel_eink_ed067kc1` | 1800×900 (electrical scan direction; 6.7" bar panel mounted 900×1800) | 8 | 2470 |
| `epd_panel_eink_es108fc2` | 1920×1080 | 16 | 1600 |
| `epd_panel_eink_es120mc1` | 2560×1600 | 16 | 1600 |
| `epd_panel_eink_ed078kc1` | 1872×1404 | 16 | 1870 |
| `epd_panel_eink_ed140tt1` | 1440×300 | 8 | 1800 |

VCOM is a property of the **individual panel**, printed on its FPC ribbon, and
is not portable even between two panels of the same model. Check yours rather
than trusting the table.

Adding a panel means adding an `epd_panel_def_t` — no driver changes. See
**PANELS.md** in the project root for a field-by-field guide and the greyscale
tuning procedure, and **EINK_DETAILS.md** for how the driver works.

---

## Performance

```
refresh_time = height × frames × row_period
```

Each pixel is driven only during its own gate slot, so refresh time is directly
proportional to drive energy. Fewer frames means a weaker image; this is physics,
not a software bottleneck.

ED103TC2 (1872×1404, 16-bit, 20 MHz, `-O2`): GC16 ≈ 358 ms, INIT ≈ 1179 ms. Per
row, ~13 µs is real DMA and ~27 µs is `esp_lcd` submit overhead — the dominant
cost is descriptor management, not data movement.
