# epdInky ESP32-P4 - FastEPD e-paper example

Drives a parallel e-paper panel with [FastEPD], which handles the 16-bit bus,
the row and gate timing, the greyscale waveforms and the TPS65185 PMIC. The
application brings up the I2C master, says which panel is fitted, and draws.

Built for an **ED103TC2** (10.3", 1872x1404, VCOM -1.25 V).

## Building

```
idf.py set-target esp32p4
idf.py build flash monitor
```

FastEPD is still in test, so it comes from the staging registry rather than the
main one. That is declared in `main/idf_component.yml`:

```yaml
ddb0515/fastepd:
  version: "2.2.5"
  registry_url: https://components-staging.espressif.com
```

## How the work is split

FastEPD owns everything on the e-paper side. The BSP is initialised with the
TPS65185 driver, the EPD GPIO block and the other on-board devices all disabled:

```c
cfg.enable.use_tps65185 = false;   /* FastEPD drives the PMIC */
cfg.enable.use_epd_gpio = false;   /* FastEPD configures these pins */
```

This is not just tidiness. Two owners racing over the panel's power sequence is
a good way to damage the glass, so there must be exactly one.

### Sharing the I2C bus

FastEPD needs I2C for the PMIC but does not insist on owning the bus. Its
`bbepI2CInit()` first calls `i2c_master_get_bus_handle(I2C_NUM_0)` and reuses
whatever is already there, creating a bus only if that fails. The BSP creates
its bus on `I2C_NUM_0` with the same pins (SDA 28, SCL 29), so bringing the
board up first means FastEPD adopts it and the two coexist.

## Panel definition

FastEPD already ships a definition for this board, `BB_PANEL_EPDINKY_P4_16`,
and its pin map matches the schematic exactly:

| Signal | GPIO | Signal | GPIO |
| ------ | ---- | ------ | ---- |
| PWRUP    | 26 | XCL       | 50 |
| SPV      | 45 | PWR_GOOD  | 27 |
| CKV      | 51 | WAKEUP    | 37 |
| XSTL     | 46 | VCOM_CTRL | 49 |
| XOE      | 47 | MODE      | 52 |
| XLE      | 48 | SDA / SCL | 28 / 29 |

So no pins have to be described by hand. The 16-bit variant is used because this
board wires all of D0..D15; the 8-bit `BB_PANEL_EPDINKY_P4` also exists and is
slower.

### Why setPanelSize() must follow initPanel()

The board definition deliberately leaves width and height at **zero**, because
the board is a carrier and the panel varies. `bbepInitPanel()` only allocates
buffers `if (pState->width)`, so with a zero size it allocates nothing and
`setPanelSize()` is what actually reserves the frame buffers and builds the
greyscale lookup tables. Calling it is not optional here.

```c
epaper.initPanel(BB_PANEL_EPDINKY_P4_16);
epaper.setPanelSize(1872, 1404, BB_PANEL_FLAG_NONE, -1250);
```

## VCOM

VCOM is a property of the glass, not the board, and it is printed on the
panel's own flexible cable. The wrong value gives a washed out image or heavy
ghosting. FastEPD writes it to the TPS65185 as `iVCOM / -10`, so the -1250 here
becomes 125 in the PMIC's 10 mV steps.

**Change `EPD_VCOM_MV` if a different panel is fitted.**

## If the image is mirrored

`EPD_FLAGS` is `BB_PANEL_FLAG_NONE`. Whether the source driver counts left to
right or the other way depends on how the glass is bonded and cannot be
detected, so if the image comes out mirrored horizontally, change it to
`BB_PANEL_FLAG_MIRROR_X`.

## 1-bit mode, not greyscale

The example runs in `BB_MODE_1BPP`. This board cannot drive the fitted panel in
4bpp - the greyscale waveform produces **nothing at all** on the glass, which
looks exactly like a dead panel rather than a mode problem. If a FastEPD example
shows a blank screen but the log reports a successful update, the mode is the
first thing to check.

Tone is faked with an ordered dither instead: `draw_dither_ramp()` turns on a
fixed fraction of pixels per block using a 4x4 Bayer threshold, which the eye
averages into apparent shades. That is how to get tonal range out of a 1bpp
panel.

`initPanel()` already leaves the mode at 1bpp, so `setMode(BB_MODE_1BPP)` here
is only being explicit. `setMode()` does not reallocate anything - it just swaps
the pixel functions - so it is safe to call at any point.

## Memory

PSRAM is required. FastEPD keeps a current and a previous plane so it can work
out which pixels actually changed, and at 1872x1404 that is a substantial
allocation even at 1bpp.

## Chip revision

`sdkconfig.defaults` pins the minimum chip revision to v1.0:

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

IDF 6.0 defaults the ESP32-P4 minimum to v3.01, which builds a bootloader this
board's v1.0 silicon refuses to run - esptool rejects it with *"requires chip
revision in range [v3.1 - v3.99]"*. The `REV_MIN_100` choice is gated behind
`SELECTS_REV_LESS_V3`, so both symbols are needed.

## Power

E-paper holds its image with no power at all, so the example drops the rails
with `einkPower(0)` once the update finishes. Leaving the high-voltage rails up
shortens the panel's life and wastes power for no benefit.

The example also calls `clearWhite()` before drawing. A panel holds whatever it
was last showing, including from before a power cycle, and drawing on top of an
unknown image leaves ghosting.

## Timing

Measured on this board with the ED103TC2:

| Step | Time |
| ---- | ---- |
| `clearWhite()` | ~1.1 s |
| `fullUpdate(CLEAR_SLOW)` | ~2.5 s |

`fullUpdate()` also accepts `CLEAR_FAST` (8 passes rather than 10), and
`partialUpdate()` refreshes a row range without a full flash.

[FastEPD]: https://github.com/bitbank2/FastEPD
