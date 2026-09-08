# epdInky ESP32-P4 — Arduino + the repository's own `epd` driver

An Arduino sketch that draws a status page on the 10.3" ED103TC2 panel using the
`epd` component in this repository, rather than FastEPD.

```
idf.py set-target esp32p4
idf.py build flash monitor
```

Expect a deep clean, a full page at about 1.4 s, then a bar drawn segment by
segment with windowed updates, and a redraw every 60 s.

## How this differs from `arduino_epd_fastepd`

|  | this example | `arduino_epd_fastepd` |
|---|---|---|
| Driver | the repo's `epd` component | FastEPD |
| Tone | 16 real grey levels (GC16) | 1bpp + dithering |
| Full update | ~1.4 s | ~2.3 s |
| BSP | used | not used — FastEPD owns I2C |
| Other board devices | available | would have to go through `Wire` |

The BSP works here because the `epd` component takes the I2C bus handle it is
given, instead of creating one. FastEPD's Arduino path calls `Wire.begin()` and
cannot share, which is why that sketch has to skip the BSP entirely.

## Partial updates: what DU is and is not for

`EPD_WAVEFORM_DU` with an `epd_rect_t` updates a window without flashing and
leaves the rest of the image untouched. That is its value.

It is also about half the cost of a full refresh — but less than you might
expect, because a DU pass still clocks **every** gate row. The gate driver is a
shift register with no random access, so "partial" means sending no-drive data
outside the window rather than skipping rows. The saving comes from DU needing
fewer frames than INIT + GC16, not from touching fewer pixels.

Measured on this board (compare µs/row rather than total time, since frame
counts move with panel temperature):

```
              us/row   of which build
DU              73          35
INIT            59           0
GC16            72          33
```

### The idle-code bias

Neither of the source driver's two "not driving" codes is actually neutral, and
they are not neutral in the same direction. Measured with the step wedge in
`duProbe()` — bands given 0, 1, 2, 5, 10 and 20 no-op DU passes over identical
white:

| code | source | effect |
|---|---|---|
| `no_drive` (0b00) | grounded | area drifts **darker** |
| `hold` (0b11) | released | area drifts **lighter** |

Both are dose-dependent and invisible for one or two passes, which is why it
only appears once several updates have accumulated — and why it is easy to
mistake for something the drawn content is doing.

Because the two bias opposite ways, the driver alternates them frame by frame so
the charge cancels rather than accumulating. `du_frames` is rounded up to an even
count for the same reason: temperature scaling can otherwise land on 7 frames
and leave one frame of drift on every update.

**DU can only darken.** A partial update can add ink and never remove it, which
is why the demo grows a bar rather than moving one. Anything that has to
disappear needs a full refresh. This is a hardware property, not a driver
limitation: driving VPOS on a sparse subset of pixels bleaches the full height of
every column it lightens, and FastEPD's `partialUpdate()` does the same thing on
this board.

## Things that had to be got right

**`Serial` must not be UART0.** On this board UART0 is GPIO37/GPIO38, the
TPS65185 WAKEUP and INT lines, so Arduino's default `Serial` would drive the PMIC
control lines as a UART. The top-level `CMakeLists.txt` sets
`ARDUINO_USB_CDC_ON_BOOT=1` so `Serial` is the USB Serial/JTAG device.

**`use_epd_gpio` is off.** That BSP helper parks the EPD pins as plain GPIO
outputs, but the `epd` component configures the same pins itself — the data lines
and CL become the i80 peripheral's. Leaving it on gives two owners for one set of
pins, and the component has to win.

**The rails come down between updates.** E-paper is bistable so the image stays,
but there is a second reason: leaving the rails up holds every pixel under a DC
bias, and over minutes that makes white areas visibly grainy.

**A bigger loop task stack.** The `epd` driver builds its rows on the calling
task's stack, and Arduino's default 8 KB is tight for that plus the core.

## A note on frame counts and temperature

The driver scales frame counts with panel temperature, so the same update can
take noticeably longer on a cold panel — 29 °C gave 100 % frames where 30 °C gave
85 %, which is a 1.3 s INIT against 1.0 s. When comparing timings, compare
`us/row` from the driver's own statistics rather than wall-clock totals, or the
temperature will look like a regression.

## VCOM

VCOM lives in the panel definition (`epd_panel_eink_ed103tc2`), not in the
sketch. It belongs to the glass — the value is printed on the panel's own
flexible cable — and a wrong value degrades the image rather than producing an
error, so it fails quietly. Keeping it out of application code stops it drifting
unnoticed.
