# epdInky ESP32-P4 - raw E Ink demo (`epd` component)

Drives a raw E Ink panel - bare glass with source and gate driver ICs and no
timing controller - using the [`epd`](../../components/epd) component. The
ESP32-P4 *is* the timing controller: every clock edge that reaches the glass
comes from the LCD i80 peripheral and a few bit-banged control lines.

Built for an **ED103TC2** (10.3", 1872x1404, 16-bit bus).

## Building

```
idf.py set-target esp32p4
idf.py build flash monitor
```

## What the application is responsible for

Very little, which is the point:

1. Bring up I2C and the TPS65185, which supplies the panel rails
2. Describe how the panel connector is wired to *this* board
3. Pick a panel definition and correct its VCOM for the glass fitted
4. Draw into a 4bpp framebuffer and refresh

Waveforms, gate scanning, row DMA and temperature compensation all belong to
the component.

## Pin selection: the traps on this board

`epd_board_config_t` comes straight from the BSP pin macros, so the only pin
needing thought is `dc_dummy`. An EPD has no command/data line, but the i80
peripheral requires a valid DC GPIO and drives it continuously through the GPIO
matrix. It idles high and never toggles, so it only has to point somewhere
harmless.

The BSP uses every GPIO except 0, 1, 24, 25 and 36 - and of those, only **36**
is actually usable:

| GPIO | Why it is not free |
| ---- | ------------------ |
| 24, 25 | `USB_DN` / `USB_DP` - the internal USB Serial/JTAG PHY |
| 0, 1 | `XTAL_32K_N` / `XTAL_32K_P` - the 32 kHz crystal X1 |
| 36 | **Usable.** Goes to R50, a 10K pull-up to 3V3, and nothing else |

**GPIO 24 is the dangerous one.** It looks unassigned in the BSP pin map, but
the schematic shows it is USB D−. Configuring it as the DC signal switches the
pad away from the USB PHY and tears the link down the instant the i80 bus is
created: the board vanishes from USB mid-boot and the log stops dead at the
`epd_display` pin summary, with no error. If a build dies exactly there, check
this pin first.

GPIO 36 is a strapping pin (boot-mode select 2 / ROM-print control), which is
fine here but worth knowing: strapping is latched at reset and the pin is then
free for normal use, and it must be **high** at reset to enter the serial
bootloader. R50 holds it high and the DC line idles high, so both the reset
state and the running state are the safe one. Do not reuse it for anything that
pulls it low.

## VCOM

VCOM is a property of the individual piece of glass and is printed on its own
FPC ribbon - it is not portable even between two panels of the same model. The
catalogue entry carries a typical value; `PANEL_VCOM_MV` in `epd_raw_main.c`
overrides it for the panel actually fitted.

Getting it wrong is not just a contrast problem: several artefacts scale with
VCOM and stay invisible when it is mis-set towards zero.

## INIT before GC16

```c
epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_INIT);
epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_GC16);
```

This is not optional. GC16 phases can only ever *darken* a pixel, so the
greyscale model assumes it starts from a white panel. Without the INIT pass the
image lands on top of whatever the panel was holding - including from before
the board was last powered - and ghosts.

## Timing

Measured on this board with the ED103TC2:

| Operation | Time |
| --------- | ---- |
| `EPD_WAVEFORM_INIT` (full clear) | ~1000 ms |
| `EPD_WAVEFORM_GC16` (16 grey levels) | ~410 ms |

Refresh time is `height x frames x row_period`. Each pixel is driven only
during its own gate slot, so the time is proportional to drive energy - fewer
frames means a weaker image. This is physics, not a software bottleneck.

## Power

E-paper is bistable, so `epd_panel_power_off()` drops the rails and the image
stays. That leaves the PMIC in STANDBY rather than sleep: dropping its WAKEUP
line powers down the I2C interface entirely, which is a system-level decision
the driver should not make on its own. Call `tps65185_sleep()` if nothing else
needs the PMIC.

## Power-good: three bugs worth knowing about

A healthy board now logs exactly this, and nothing else:

```
TPS65185:    All power rails up and good after 250 ms (PG=0xFA)
epd_display: PMIC rails good after 0 ms: PG=0xFA
```

Getting there needed three fixes, each of which produced a confident but wrong
message:

**The check raced the sequencer.** The TPS65185 raises its rails in a
programmed order with a delay between strobes. Measured here, VPOS/VEE/VNEG are
good ~150 ms after PWRUP but **VDDH not until ~300 ms**. Both `power_up()` and
`epd_panel_power_on()` sampled once after a fixed delay and declared a missing
gate rail on a perfectly healthy panel. Both now poll until the rails are up,
with a timeout that only applies when something is actually wrong.

**PWR_GOOD could never read high.** The TPS65185's PWR_GOOD is an open-drain
output - it pulls low when the rails are bad and simply releases when they are
good - so it needs a pull-up. This board routes it straight from the PMIC to
GPIO 27 with no resistor, and the driver configured the pin with its internal
pull-up disabled, so it floated low forever. The driver now enables the
internal pull-up rather than depending on the board having fitted one.

**The two checks disagreed because they read different things.** `power_up()`
judged the rails from the PWR_GOOD pin while the EPD driver read the PG
register, which is how a single run could print *"Power rails not good ...
(VB=ok VDDH=ok VN=ok VPOS=ok VEE=ok VNEG=ok)"* - a complaint and its own
refutation on one line. Both now read the PG register, which also names the
individual rails, so a real failure says *which* rail is missing.
