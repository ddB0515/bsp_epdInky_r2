# MIPI-DSI camera preview (ESP-IDF)

Shows the SC2336 camera live on the D320C2403V1 MIPI-DSI panel, with an LVGL
interface and capacitive touch.

The camera runs at 1280x720 YUV420 and the PPA scales and colour-converts each
frame straight into the display's frame buffer as RGB888. Nothing is copied in
between: one hardware pass reads the camera buffer and writes the buffer the
panel is scanning out of.

```
Camera 1280x720 YUV420 --> PPA (scale + YUV->RGB888) --> DPI frame buffer 1024x768
                                                         LVGL draws the bars
```

## Hardware

| | |
|---|---|
| Panel | D320C2403V1-MIPI, 3.2", 1024x768, MIPI-DSI video mode |
| Driver | JD9168, 2 data lanes |
| Touch | GT967 over I2C at 0x5D |
| Backlight | SGM37604A over I2C at 0x36 |
| Adapter | TCA6408 expander at 0x20 |

The panel plugs into FPC1 through an adapter board, and **that adapter is what
makes the display work**. It carries a second TCA6408 (0x20; the mainboard's is
0x21) which gates everything:

| Expander pin | Net | Controls |
|---|---|---|
| P0 | `GPIO_EN` | AP2281 load switch -> `LCD_VDD` |
| P1 | `LCD_RES` | JD9168 reset, active low |
| P2 | `BL_EN` | SGM37604A hardware enable |

So the bring-up order is not optional: power the panel, release its reset, then
enable the backlight controller. Before P2 goes high the SGM37604A does not
acknowledge on I2C at all, and before P0 the panel is unpowered — a bus scan at
that point shows neither 0x36 nor 0x5D and looks exactly like an unplugged
display.

There is no panel reset GPIO on the main board, which is why the JD9168 is
created with `reset_gpio_num = -1` and reset through the expander instead.

## Build and run

```sh
. ../../export-idf.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

Colour bars appear for about a second and a half at startup. They come from the
DSI controller itself, so if you see them the link, the panel and the backlight
are all working regardless of what the application does next — a useful first
check when bringing up a new panel.

## Screen layout

The camera is 16:9 and the panel is 4:3, so a full-width preview is 1024x576
with a 96-pixel bar above and below.

```
+--------------------------------------------------+  y=0
|  epdInky  SC2336        luma 103        15.0 fps |  top bar, LVGL
+--------------------------------------------------+  y=96
|                                                  |
|              camera preview 1024x576             |  PPA writes here
|                                                  |
+--------------------------------------------------+  y=672
|  [Mirror]  [Flip]              brightness ====== |  bottom bar, LVGL
+--------------------------------------------------+  y=768
```

Those bars are not decoration — they are what makes two writers sharing one
frame buffer safe. Every LVGL widget lives in a bar, so a redraw can never land
on a pixel the PPA owns, and neither has to wait for the other. LVGL runs in
partial render mode, so it only ever touches areas that actually changed.

## Frame rate, and the one real trade-off

The PPA manages roughly **25 Mpixel/s** for YUV420 to RGB888 with rescaling, and
the cost tracks the *source* size rather than the destination. Measured on this
board, both scaling into the same 1024x576 window:

| Sensor mode | PPA time | Preview |
|---|---|---|
| 1280x720 | 33.9 ms | 15.0 fps |
| 640x480 | 13.4 ms | 36.7 fps |

Two experiments pinned that down. Halving the *output* area changed the time
barely at all (33.9 -> 32.5 ms), and writing to a scratch buffer instead of the
scan-out frame buffer made no difference either, which rules out both output
bandwidth and contention with the DPI scan-out. Shrinking the *input* is what
moves the number.

720p is the default because it is the closest match to the 1024x576 window and
gives the sharpest picture. If a smoother viewfinder matters more than detail,
change `PREVIEW_SENSOR_W/H` in `dsi_camera_preview_main.c` to 640x480 — the
sensor mode switching handles the rest.

## Touch

The GT967 is a Goodix GT9xx part and speaks the same register protocol as the
GT911, so `espressif/esp_lcd_touch_gt911` drives it unmodified. The driver
confirms the part on the log line:

```
I (3482) GT911: TouchPad_ID:0x39,0x36,0x37
```

Those bytes are ASCII `9`, `6`, `7`.

Touch is treated as optional: if it does not answer, the preview and the UI
still run, just without input.

## LVGL is driven directly, not through esp_lvgl_port

`espressif/esp_lvgl_port` does not compile against ESP-IDF 6.0. It sets
`cbs.on_frame_buf_complete` for IDF >= 5.5, but 6.0 renamed that member back to
`on_refresh_done`, so the build fails in the component itself:

```
error: 'esp_lcd_dpi_panel_event_callbacks_t' has no member named 'on_frame_buf_complete'
```

This was checked against port 2.9.0, the newest published version. Rather than
patch a managed component — which gets silently overwritten on the next
dependency resolve — LVGL is driven directly in `app_ui.c`. That is only a few
dozen lines, and it gives exact control over which pixels LVGL is allowed to
touch, which is what keeps it out of the PPA's way.

## Components

Two new components were made from the vendor drivers:

- `components/esp_lcd_jd9168` — the JD9168 panel driver
- `components/sgm37604a` — the backlight controller

The backlight driver had a brightness bug worth noting: the 12-bit level is
split across two registers as a low nibble and a high byte, and writing the
level straight into the MSB register truncates it to 8 bits instead of shifting
by 4. The panel then appears to wrap and dim again every time the level crosses
a multiple of 256. `sgm37604a_set_brightness()` now shifts properly.

## Next steps

- Snapshot to SD from the touch UI, reusing `app_snapshot.c` from the web server
  example
- Wire the resolution chooser into the UI, since the sensor mode switching is
  already there
- Overlay widgets on the video area, which needs LVGL to composite rather than
  write straight to the frame buffer

## DSI timings: 800 Mbps / 45 MHz, not the vendor macro's 900 / 50

The JD9168 vendor header ships `lane_bit_rate_mbps = 900` and
`dpi_clock_freq_mhz = 50`. **This panel does not work at those rates.** The DSI
bridge never scans the frame buffer out, so the display holds whatever was last
on it and nothing the application draws ever appears.

What makes this failure hard to recognise is that the DSI test pattern
(`esp_lcd_dpi_panel_set_pattern`) still renders perfectly. That pattern is
generated inside the DSI host, downstream of the bridge, so it exercises the
PHY, the panel and the backlight but says nothing about whether frame buffer
data is flowing. Colour bars therefore look like proof that the display works
when in fact the entire data path is dead.

The reliable signal is VSYNC. `app_display_selftest()` registers
`on_refresh_done` and counts interrupts for half a second:

- **~27 in 500 ms (~54 Hz)** - the panel is being refreshed from the frame buffer
- **0** - scan-out is not running, and nothing drawn will ever be visible

The selftest is left enabled because it costs 500 ms at boot and turns an
invisible, badly-disguised failure into one clear line in the log.

## Resolution buttons change the sensor, not just the scaling

Both preview modes are real SC2336 hardware modes, so 640x480 is genuinely a
smaller capture rather than a cropped or downsampled 1280x720. That is why it
is faster: the PPA's cost tracks the *source* pixel count, not the destination.

Both still pass through the PPA, because the preview rectangle is a fixed
800x450 and neither sensor mode matches it:

| Sensor mode | Aspect | Scale | Drawn as | Bars          | Rate    |
| ----------- | ------ | ----- | -------- | ------------- | ------- |
| 640x480     | 4:3    | 15/16 | 600x450  | pillarboxed   | 37 fps  |
| 1280x720    | 16:9   | 10/16 | 800x450  | none          | 15 fps  |
| 1920x1080   | 16:9   |  6/16 | 720x405  | thin, all round | 7.5 fps |

The scale is always a whole number of sixteenths. The PPA quantises the scale
factor to 1/16 steps and derives its output size from that quantised value, not
from the rectangle it was handed. Asking for 800x450 from a 1920x1080 frame
makes it write just 720x405 and leave the previous image visible along the right
and bottom edges. Choosing the scale in sixteenths first and deriving the
destination from it keeps the two in agreement, so 1080p is drawn slightly
smaller and centred rather than full-width with stale strips.

1280x720 is the default: it matches the window's shape and balances detail
against rate. 1920x1080 costs 126.9 ms/frame because the PPA has 2.25x more
source pixels to read, which is the whole cost of the operation.

The scale factor is taken from whichever axis is the tighter fit and the result
is centred, so the 4:3 mode keeps its proportions. Scaling both axes to fill the
frame would stretch it about 33% too wide. The bars are painted once when the
shape changes rather than every frame, since the PPA only writes the video
rectangle itself.

## Saving stills

The **Save to SD** button under the brightness slider writes a JPEG to
`/sdcard/snapNNNN.jpg`, picking the next free number. Each file is read back
after writing and checked for a complete JPEG (SOI and EOI markers) before it is
reported as saved.

The capture and the write both happen in the preview task rather than in the
button handler. That task already owns the camera, and encoding from the LVGL
task would contend for the same capture queue. The button only raises a flag,
which the preview loop picks up between frames.

Two things this has to get right:

- The encoded frame comes from the same queue as the preview frames, so it must
  be released afterwards. Miss that and every subsequent capture fails with
  `previous frame was not released`.
- A missing card is not fatal. `app_snapshot_init()` reports the card as
  unavailable, the button is created disabled, and the status line reads
  "No SD card" instead of failing on every press.

## Why the preview runs at 15 fps, not 30

The rate has nothing to do with LVGL, which only redraws the static UI when
something is touched. It is set entirely by how long one preview frame takes.

Measured at 1280x720:

```
PPA 42.0 ms | camera wait 24.3 ms | loop 66.3 ms -> 15.0 fps
```

66.3 ms is almost exactly two camera frame periods (2 x 33.3 ms). This is the
key point: **the frame rate is quantised, not gradual.** The sensor delivers a
frame every 33.3 ms, so if a loop iteration cannot finish inside that window it
misses the next frame entirely and waits for the one after. Anything over 33.3 ms
lands on exactly half rate. The "camera wait" is simply idling until the next
frame arrives.

So 42 ms of PPA time does not cost 25% of the rate - it costs 50%.

### What the PPA actually costs

Its throughput tracks the *source* pixel count, at roughly 22 Mpixel/s:

| Source    | Output   | PPA time | Mpixel/s |
| --------- | -------- | -------- | -------- |
| 1280x720  | RGB888   | 49.9 ms  | 18.5     |
| 1280x720  | RGB565   | 42.0 ms  | 21.9     |
| 640x480   | RGB565   | 14.4 ms  | 21.3     |

Reaching 30 fps at 720p would need 921600 pixels inside 33.3 ms, or
**27.7 Mpixel/s** - about 27% more than the block delivers. That is why 720p
sits at 15 fps: it is a throughput limit of the PPA, not a configuration
mistake.

RGB565 was tried for the frame buffer, since it cut PPA time by 16% and halved
the DSI scan-out load (127 MB/s at 24 bits, 85 MB/s at 16). **It does not work
on this panel.** The DPI reports a healthy 54 Hz of VSYNC and scan-out is
plainly running, but nothing reaches the glass - the JD9168 init sequence
configures 24-bit pixels and there is no documented 16-bit variant. Tested at
both DSI lane rates, so it is the panel and not the timings. The frame buffer
therefore stays RGB888 and that bandwidth saving is unavailable.

Note this is only about the *display* frame buffer. The JPEG path still converts
to RGB565, because that is what the hardware encoder consumes.

### Getting closer to 30 fps

- **Use 640x480.** Fewer source pixels is the only lever that moves the PPA
  much, and it is why the small mode is noticeably smoother. Measured at
  14.4 ms of PPA time against 42 ms for 720p.
- **Have the ISP produce the preview size directly.** The camera pipeline can
  emit RGB565, so if it could also scale, the PPA would drop out of the loop
  entirely. This is the only route to 720p at 30 fps and has not been tried.
- Raising the DSI clock does not help: the PPA is the bottleneck, not scan-out.

## The stats overlay in the bottom-right corner

Shows `CPU n/n%  LVGL n fps`, updated once a second.

### Why not LVGL's built-in perf monitor

LVGL ships one (`LV_USE_PERF_MONITOR`, which already defaults to
`LV_ALIGN_BOTTOM_RIGHT`), and enabling it is a one-line change. It is the wrong
tool here. With `LV_USE_OS = LV_OS_NONE` its CPU figure comes from
`lv_timer_get_idle()`, which measures how busy the **LVGL timer handler** is,
not the chip. Its FPS counts LVGL refresh cycles. In this example the PPA writes
the video straight into the frame buffer and LVGL never sees it, so the built-in
monitor would sit near 0% and a couple of FPS while both cores and the PPA were
working hard - technically true, but it looks broken and says nothing useful.

### What the numbers here mean

- **CPU** is real, taken from the FreeRTOS idle-task run-time counters. Busy
  time is not measured directly; the idle counter is sampled each second and
  whatever is missing from the wall-clock interval was real work. Needs
  `FREERTOS_GENERATE_RUN_TIME_STATS`, `FREERTOS_USE_TRACE_FACILITY` and
  `FREERTOS_VTASKLIST_INCLUDE_COREID`, all set in `sdkconfig.defaults`. The
  counters are driven by esp_timer, so they are already microseconds.
- **LVGL fps** counts real render passes (`LV_EVENT_RENDER_READY`), not refresh
  ticks. `LV_EVENT_REFR_READY` fires every refresh period even when nothing was
  redrawn and would report a meaningless steady 30.

A typical idle reading is `CPU 7/0%  LVGL 2 fps`. Both numbers being low is
correct, and it is the useful part: it shows the 15 fps preview limit is not the
CPU. The cores are almost idle because the camera, PPA and DSI all move data by
DMA - the ceiling is PPA throughput, as measured in the frame-rate section
above.
