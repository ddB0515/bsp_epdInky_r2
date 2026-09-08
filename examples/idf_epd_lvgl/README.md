# epdInky ESP32-P4 - LVGL on a raw E Ink panel

**Status: working.** LVGL drives the 1872x1404 ED103TC2 through the `epd`
component, with a refresh policy that keeps a 1.4 s panel update from being paid
more often than it has to be.

## The idea

LVGL and e-paper want opposite things. LVGL assumes it can flush a small dirty
rectangle cheaply and often; this panel needs a full INIT + GC16 costing about
1.4 s, and its DU partial update is unusable (see the raw EPD example - a source
line that drives VPOS retains the charge and bleaches the rest of its column).

So the two are decoupled:

```
LVGL  ->  L8 shadow buffer  ->  [refresh policy]  ->  4bpp  ->  panel
```

LVGL renders at its own pace into an 8-bit greyscale buffer in PSRAM and never
waits for the glass. The flush callback only copies the band and records that
something changed. A separate policy decides when to pay for a refresh:

- **settle_ms** - refresh once the screen has stopped changing. One button press
  produces several flushes (pressed, released, label), and refreshing on each
  would cost seconds and flash the screen repeatedly.
- **max_defer_ms** - but never let a change wait longer than this, so a UI that
  never goes quiet (a clock, a spinner) still reaches the glass.

## Why L8

The panel is 4bpp greyscale, and `LV_COLOR_FORMAT_L8` is 8-bit greyscale, so the
conversion is one shift per pixel:

```c
dst[i >> 1] = (src[i] & 0xF0) | (src[i + 1] >> 4);
```

Rendering RGB565 would mean a colour conversion and a tone-mapping pass on every
flush, for a panel that cannot show colour.

## Configuration lives in Kconfig, not lv_conf.h

The ESP-IDF LVGL component sets `LV_CONF_SKIP` and takes its configuration from
Kconfig. A `main/lv_conf.h` in the project is **silently ignored** - an easy way
to spend an afternoon wondering why a setting has no effect. Everything is in
`sdkconfig.defaults`:

```
CONFIG_LV_COLOR_DEPTH_8=y
CONFIG_LV_DRAW_SW_SUPPORT_L8=y
CONFIG_LV_FONT_MONTSERRAT_48=y
```

**The rails must come down between updates.** This is the one that produces a
visible defect rather than a crash, so it is easy to ship by accident. Leaving
the panel powered between refreshes looks like an obvious optimisation - it
saves the power-up sequence on every update - but with the rails up, VCOM sits
live at its operating voltage while the source lines rest near ground. Every
pixel then sees a continuous DC field of the full VCOM magnitude. Over a few
minutes the pigment drifts under it and uniform white areas go visibly grainy.

`epd_display_power_off()` already goes to considerable lengths to neutralise
exactly this bias at shutdown; holding the rails up simply sustains it instead,
for minutes rather than one scan.

Two things make it easy to miss:

- The magnitude scales with VCOM, so it is invisible on a panel whose VCOM is
  mis-set near zero and only appears once VCOM is *correct*. It showed up here
  immediately after VCOM was corrected to 1250 mV.
- It develops over minutes, so a demo that runs for thirty seconds and exits
  never shows it.

The cost of doing it properly is real: a refresh goes from about 1.4 s to about
3.1 s, because the power-down sequence (neutralising scan, global discharge,
post-discharge wait) is roughly 1.3 s and is tuned to leave a panel safe to sit
unpowered indefinitely. `keep_rails_on` in `app_epd_lvgl_config_t` restores the
old behaviour for a UI that updates more or less continuously and never idles
under bias.

## Things that had to be got right

**PARTIAL, not DIRECT.** Handing LVGL the shadow buffer directly removes the
copy and looks obviously better. LVGL 9.5 hangs inside `lv_timer_handler()` the
second time round with an L8 single-buffer direct display. PARTIAL is the
conventional path and is what the library is exercised against.

**A dedicated task with a big stack.** LVGL's rendering call depth plus the EPD
driver's row construction overflows the 3584-byte default main task stack, and
reports a *stack protection fault* - which reads like memory corruption rather
than the sizing problem it is. 24 KB is comfortable.

**The idle task watchdog has to be told to stand down.** A refresh is roughly a
second of uninterruptible hardware sequencing on one task, which starves that
core's idle task. The default watchdog treats that as a fault and resets the
chip, so the startup clean never finishes and the board reboots forever.

**The panel definition must outlive the call that used it.** This one cost a
lot of time, so it is worth spelling out. `epd_display_panel_create()` used to
store the `epd_panel_def_t *` it was given, and the example passed a local from
`app_main`. Everything worked for as long as `app_main` was alive; the moment it
returned and its stack was reused, the driver was reading a definition made of
garbage, and a refresh would wander off mid-frame. The component now copies the
definition, so a caller's stack local is fine.

The reason it took so long to find is that the symptom appeared nowhere near the
cause. It looked like *"refreshes hang when called from the LVGL task"*, because
the arrangement that moved refreshes onto the LVGL task was also the arrangement
in which `app_main` returned. Two unrelated things changed together, and the
wrong one got the blame - which led to a long detour through core affinity, task
priority and bus mutex instrumentation, none of which was ever involved.

Two lessons worth keeping:

- **Change one thing at a time.** Moving the refresh *and* letting `app_main`
  return were a single edit, and that is what made the correlation misleading.
- **`Returned from app_main()` in the log is a fact, not noise.** It was sitting
  immediately before the stall in every failing capture. Reading the log line
  before the silence, rather than theorising about the silence, is what finally
  cracked it.

## A note on flashing this board

Flashing is unreliable, and its failure mode imitates a firmware bug. A bad
write produces a crash in ROM *before* the second-stage bootloader prints
anything:

```
load:0x4ff33ce0,len:0x15e0
Guru Meditation Error: Core 0 panic'ed (Store/AMO access fault)
```

Identical registers every time, and it can also fail to boot on one reset and
succeed on the next from the same image. If the board appears dead after a
flash, reset it a few times before believing anything about the code. Several
rounds of this session were spent debugging firmware that was never running.

## Try it

```
idf.py set-target esp32p4
idf.py build flash monitor
```

Expect a deep clean, the first screen at about 8 s, then a short scripted
sequence: the counter goes up three times, down once, the switch toggles off and
back on, and the slider moves twice. One refresh per step, 5 s apart, then the
panel goes quiet.

`DEMO_EMULATE_TAPS` in `epd_lvgl_main.c` drives that sequence by sending
`LV_EVENT_CLICKED` to the real buttons rather than calling the handlers, so the
emulated path and a touch path are the same path. Set it to 0 on a board with a
touchscreen. It is worth keeping even where touch works: a static screen proves
nothing, whereas a counter that visibly counts proves the whole chain from an
LVGL event through the shadow buffer to the glass.

The step interval has to exceed `settle_ms` plus a refresh - which is about
3.1 s here, since the rails are cycled around each update - or the settle policy
does its job and coalesces several steps into one, correct behaviour that hides
the intermediate values.

`MINIMAL_UI` in the same file reduces the screen to a single label, which is the
quickest way to tell a bridge problem from a widget problem.

