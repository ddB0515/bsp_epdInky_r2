# MIPI CSI camera web server (ESP-IDF)

Streams the SC2336 camera over Wi-Fi two ways: an **MJPEG preview over HTTP**
with live camera controls, and an **H.264 RTSP stream** for VLC, ffmpeg or NVR
software, at any of the sensor's four resolutions. While the preview is open,
stills can be saved to the board's SD card or downloaded straight to your
device.

Built entirely on the official Espressif components — `espressif/esp_video`
(V4L2 interface, MIPI-CSI device, ISP), `espressif/esp_cam_sensor` (SC2336
driver) and `espressif/esp_h264` (hardware encoder) — plus this repository's
BSP for I2C and Wi-Fi. The RTSP/RTP server is implemented here, because
Espressif publish no RTSP component.

```
:80   GET /               control page
:80   GET /api/controls   list controls with ranges and current values (JSON)
:80   GET /api/control    set one control:  ?id=<id>&value=<v>
:80   GET /api/reset      restore every control to its default
:80   GET /api/snapshot   save a still to the SD card (JSON reply)
:80   GET /api/photo      return a still as image/jpeg, for download
:81   GET /stream         multipart/x-mixed-replace MJPEG stream, 1280x720
:80   GET /api/resolution choose the sensor mode (?w=&h=)
:8554     /stream1..4     RTSP, H.264, one mount per sensor mode
```

**Only one stream runs at a time.** There is a single capture queue and a
single encoder, so opening the RTSP stream stops the MJPEG preview and vice
versa. The handover is immediate — the displaced consumer notices it no longer
owns the camera and closes cleanly.

The MJPEG stream is on a **second port on purpose**. `esp_http_server` handles
requests on a single task per instance, and the stream handler blocks for as
long as the client is connected — sharing one instance would stall every
control request until streaming stopped.

## Build and run

Create `main/wifi_credentials.h` first (it is gitignored):

```sh
cp main/wifi_credentials.h.example main/wifi_credentials.h
. ../../export-idf.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

The console prints the addresses to open:

```
I (7624) camera_web:  Camera ready at 1280x720
I (7625) camera_web:  Preview  http://192.168.1.83/  (press "Open stream")
I (7625) camera_web:  RTSP     rtsp://192.168.1.83:8554/stream  (H.264 1280x720)
```

## RTSP

Four mounts, one per sensor mode:

| Mount | Resolution | Frame rate |
|---|---|---|
| `/stream1` | 1920x1080 | ~15 fps |
| `/stream2` | 1280x720 | ~30 fps |
| `/stream3` | 800x800 | ~30 fps |
| `/stream4` | 640x480 | ~36 fps |

```sh
ffplay -rtsp_transport tcp rtsp://192.168.1.83:8554/stream2
vlc rtsp://192.168.1.83:8554/stream1
```

Connecting to a mount switches the sensor into that mode, so only one can play
at a time — as does the preview, which shares the same camera.

Both RTP-over-UDP and RTP-interleaved-over-TCP are supported; ffmpeg defaults
to UDP. **Prefer TCP.** UDP works but has no retransmission, so a dropped
packet shows up as a corrupt macroblock ("error while decoding MB"); over TCP
the same stream decodes cleanly.

The server implements the subset of RFC 2326 that real clients use — OPTIONS,
DESCRIBE, SETUP, PLAY, TEARDOWN and the keep-alive methods — and packetises
H.264 per RFC 6184, fragmenting oversized NALs with FU-A. SPS and PPS are
cached from the first keyframe and published in the SDP as
`sprop-parameter-sets`, so clients can decode from the first packet.

Measured: H.264 Constrained Baseline, 1280x720, ~30 fps, 2.49 Mbit/s over a
10-second capture. MJPEG at the same resolution runs about 10 Mbit/s, and at
1080p would need 25-35 Mbit/s — more than the SDIO-attached Wi-Fi radio can
carry. That is why RTSP uses H.264.

### Rate control needs an explicit QP window

Setting `V4L2_CID_MPEG_VIDEO_BITRATE` on its own does nothing useful. The
esp_video H.264 device defaults to `min_qp = 25`, `max_qp = 26` — a one-step
window that leaves the rate controller nowhere to go, so the encoder runs at a
fixed QP and produces whatever bitrate that costs. Measured with the defaults:
**9.7 Mbit/s** against a 2.5 Mbit/s target, at the same resolution and frame
rate.

`app_h264.c` therefore sets `V4L2_CID_MPEG_VIDEO_H264_MIN_QP` and `MAX_QP` to
18 and 40. With that window the target is hit almost exactly.

## Stills

Once the preview is open, two buttons appear beneath it. They are hidden until
then, because a still is the frame you are currently looking at.

- **Save to SD card** writes `/sdcard/snapNNNN.jpg` on the board. The name is
  8.3-safe on purpose, so it works whether or not FATFS long filename support
  is enabled, and the next free index is found with `stat()` so a reboot never
  overwrites earlier shots. The button is hidden entirely when no card is
  mounted (`/api/controls` reports `"sd": false`).
- **Download to device** saves the image through the browser instead, named
  `epdinky-YYYYMMDD-HHMMSS.jpg`. The filename is chosen client-side because the
  browser knows the real date and time, while the board's RV-3028 RTC may never
  have been set.

Both take the picture the same way, and how the frame is obtained depends on
what is running:

- **While the MJPEG preview is streaming**, the streaming loop already holds a
  freshly encoded JPEG, so the request asks it to hand the next frame over.
  Nothing competes for the camera, and the image is exactly what the viewer was
  looking at.
- **When nothing is streaming** (the endpoints can still be called directly),
  the camera is taken for a single shot after discarding a few frames so
  auto-exposure can settle. That warm-up matters: without it the first frame is
  often mis-exposed and noisy, which produced a 227 KB file where a settled
  frame is about 38 KB — noise does not compress.

The streaming loop only ever **copies** the frame; the SD write or the socket
send happens on the requesting task. Doing the write inside the loop would
stall the stream for its whole duration.

Stills are **refused with 409 while RTSP is streaming**: the camera is
producing H.264, so there is no JPEG to hand over, and taking it would
interrupt the viewer.

Every SD file is read back after writing and checked for the JPEG SOI/EOI
markers and the expected length. A file that fails is deleted and an error
returned, so a short write to a flaky card cannot leave a truncated image
behind.

The card is mounted **after** Wi-Fi connects, not during board init. The P4 has
a single SDMMC controller and esp-hosted claims it for the C6 radio, so the
order matters; a missing card is also non-fatal, leaving the camera usable.

### Upstream bug: crash when no card is fitted

Running with no SD card used to panic the board a few milliseconds after the
mount failed, with `Load access fault` and `MTVAL 0x9c`. The backtrace pointed
at `sd_host_isr()` rather than anything nearby, which is the giveaway:

```c
sd_host_sdmmc_slot_t *slot = ctlr->slot[ctlr->cur_slot_id];   /* no NULL check */
...
if (slot->cbs.on_trans_done)                                  /* offset 0x9c */
```

`cur_slot_id` is set per transaction and is never reset when a slot is removed,
while removing a slot sets `ctlr->slot[id] = NULL`. A failed mount does
transactions on slot 0, so `cur_slot_id` is 0, and IDF's cleanup then releases
slot 0 — but the controller stays alive because esp-hosted still holds slot 1
for the radio. The next SDIO interrupt, generated by ordinary Wi-Fi traffic,
walks into the NULL slot.

`bsp_sdcard_mount()` works around it by giving the host a `deinit_p` that
**retains** the slot instead of releasing it, so that pointer can never go NULL
while the controller is live. Nothing else on this board wants those pins, and
card detect is not routed, so the card is only ever mounted at boot; if mount is
called again the retained slot is handed back first.

## Resolution and sensor modes

The preview page has a **Resolution** dropdown, built from the modes the sensor
reports rather than a hardcoded list, so it can never offer something the
hardware lacks. On the SC2336 that is **1920x1080, 1280x720, 800x800 and
640x480**. There is no 800x600 mode; 800x800 is the sensor's square format and
640x480 its 4:3 one, both cropped internally rather than scaled, so neither is
distorted.

Changing resolution switches the *sensor mode*, because the H.264 encoder can
only be fed the sensor's native output. Both the preview and the RTSP mounts do
this, which is also why only one stream can run at a time.

Exposure limits are mode-specific — the maximum is bounded by the mode's frame
length — so the control table is rebuilt after every switch. Measured maxima:
1494 at 720p, 1244 at 640x480, 1194 at 1080p, 994 at 800x800.

### How the mode is switched, and why it needs private headers

esp_video offers no supported way to change resolution at runtime:

- `VIDIOC_S_FMT` takes the size from the active sensor mode. It *validates* the
  width and height you pass against that mode and rejects a mismatch, but it
  will not change it.
- `VIDIOC_ENUM_FRAMESIZES` only ever reports the current size.
- `VIDIOC_S_SENSOR_FMT` does change it, but wants an `esp_cam_sensor_format_t`
  that stays valid forever (the driver stores the pointer), and the real ones
  are `static` inside `sc2336.c`.

The official answer is the `video_custom_format` example: declare your own
format descriptors, register tables and all. For four modes that is roughly 700
lines of copied vendor data that rots silently when the component updates.

`app_sensor.c` asks the driver for its own table instead.
`esp_cam_sensor_query_format()` returns pointers straight into that static
array — exactly what `VIDIOC_S_SENSOR_FMT` wants — but it needs the sensor
handle, which is only reachable through esp_video's private headers:

```c
struct esp_video *v = esp_video_device_get_object(CSI_NAME);   /* "MIPI-CSI" */
esp_cam_sensor_device_t *sensor = VIDEO_DEVICE_COMMON(v)->cam.sensor;
```

That private surface is deliberately tiny and confined to `app_sensor.c`.
Including the real headers rather than hardcoding struct offsets means a
component update that changes the layout breaks the build instead of corrupting
memory. If `esp_video` ever exposes a supported way to enumerate and select
sensor modes, that file is the only one that needs to change.

## Idle low-power mode

With no MJPEG viewer, no RTSP session and no web request for **60 s**
(`IDLE_TIMEOUT_S`), the board shuts down and sleeps:

1. RTSP and HTTP servers stop, so no new client can arrive mid-teardown
2. the camera is suspended - `VIDIOC_STREAMOFF` stops the sensor and frees the
   frame buffers
3. Wi-Fi is torn down
4. the P4 enters light sleep

Press the **user button (SW4, GPIO35)** to wake. Everything comes back in
place, without a reboot: verified as `wake_reason=gpio`, `wakes=1` and no
change of reset reason, with RTSP, the preview and SD snapshots all working
immediately afterwards.

The page's status polling deliberately does **not** count as activity, and the
page only polls while streaming — otherwise a forgotten browser tab would hold
the board awake for ever.

`/api/controls` reports `wakes`, `wake_reason`, `reset`, `resume_fail` and
`resume_err`. Those exist because the USB console does not survive light sleep
(below), so HTTP is the only way to see what happened across a nap. The failure
fields live in `RTC_NOINIT_ATTR` memory so they survive the restart a failed
resume triggers — `RTC_DATA_ATTR` is *not* enough, as it is re-initialised on a
software reset.

### Light sleep rather than deep sleep

Deep sleep needs an RTC GPIO (0–15) for a reliable wake, and GPIO35 is not one.
Light sleep also keeps RAM, so waking is quick and the pipeline can be resumed
rather than rebuilt.

### What could not be powered down, and why

**The ESP32-C6 radio stays powered.** Holding `C6_CHIP_PU` (GPIO54) low would be
the single biggest saving, since the C6 is a whole second chip. It cannot be
done: esp-hosted treats a co-processor reboot as fatal desynchronisation and
calls `hosted_restart_host()` → `esp_restart()`. Resetting the C6 around sleep
therefore rebooted the P4 on every wake, showing up as
`esp_reset_reason() == ESP_RST_SW` with no error of our own recorded.

esp-hosted does have its own host power-save API
(`esp_hosted_power_save_start()`), but it is built for the *slave* waking the
*host* through an RTC GPIO and only supports deep sleep, so it does not fit a
button wake either. Stopping the Wi-Fi stack at least leaves the C6 with no
traffic to service.

**The camera is suspended, not deinitialised.** `esp_video_deinit()` cannot be
undone — it leaves the ISP video device registered, so the next
`esp_video_init()` fails with:

```
E esp_video: Failed to register video VFS dev name=video20
E esp_video_init: Failed to create hardware ISP video device
```

Confirmed by tearing the camera down and bringing it straight back with the
console attached. Suspending is better anyway: `VIDIOC_STREAMOFF` stops the
sensor and releases the buffers, which is where the power goes, and resuming is
two ioctls rather than a full re-detection.

**The SD card is left mounted.** Powering it down saves very little, and the
card did not reliably initialise again afterwards, which left snapshots broken
for the rest of the session.

### The board cannot be flashed while asleep

The P4 cannot keep USB alive in light sleep —
`SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP` is commented out in the SoC header
(IDF-6395) — so esptool cannot reset a sleeping board into the bootloader, and
the serial console stays dead after waking until the next reset.

**Hold the user button while the board boots** to disable idle sleep for that
session. That is the reliable way to get a flashable board back, and worth
knowing before it catches you out.

## Why H.264 cannot be scaled

Two hardware findings, both verified on this board, decide the design.

**The PPA scaler and the hardware H.264 encoder cannot run at the same time.**
Once the encoder is open, the PPA's completion interrupt never arrives and
`ppa_do_scale_rotate_mirror()` blocks forever. Verified in isolation: the PPA
alone works (it is what scales and colour-converts the MJPEG preview), the
encoder alone works, and together they deadlock on the first transaction.

**The CSI driver cannot change resolution at runtime.** `esp_video`'s
`VIDIOC_S_FMT` handler ignores the width and height you pass — the size comes
from whichever sensor mode is active. Changing modes needs an
`esp_cam_sensor_format_t` that lives in a `static` array inside `sc2336.c`,
with no public accessor and no index-select ioctl.

Together those mean an H.264 stream can only ever be the sensor's native
resolution — the one size needing no scaling — which is why each RTSP mount
switches the sensor rather than scaling a shared frame.

The MJPEG preview is unaffected by the PPA conflict, because it never runs
while the encoder is open; it uses the PPA for the YUV420 to RGB565 conversion
the JPEG encoder needs.

## Pixel formats

Capture is always **YUV420** in the P4's native `OUYY_EVYY` layout, which is
exactly what the H.264 encoder consumes — so the camera-to-encoder path does no
pixel conversion at all.

The MJPEG path does need a conversion, because this board's ESP32-P4 is silicon
revision **v1.0** and its JPEG encoder only gained YUV420 input on **v3.0**
(`JPEG_ENCODE_IN_FORMAT_YUV420` is compiled out below that revision). The PPA
handles the YUV420-to-RGB565 conversion, and can downscale in the same pass if
the preview is ever served smaller than the sensor mode.

## Live controls

The page builds its control panel from `/api/controls`, so it only ever shows
what this sensor actually supports. On the SC2336 that is:

| Control | Range | Notes |
|---|---|---|
| Auto exposure | on/off | software AE loop, on by default |
| Brightness target | 40–200 | what the AE loop aims for |
| Exposure | 8–1244 | manual; only settable with AE off |
| Gain | 0–192 | manual; only settable with AE off |
| Mirror / Flip | on/off | horizontal and vertical |
| JPEG quality | 10–100 | trades image quality against bandwidth |
| Red gain / Blue gain | 1.00x–4.00x | ISP white balance |

Setting exposure or gain while auto exposure is on returns **409 Conflict** —
the loop would immediately overwrite the value, so the UI disables those
sliders instead of letting them silently do nothing.

Everything can be changed live while the stream is running.

The JPEG output buffer is sized for the **worst case**, not the typical one:
two bytes per pixel plus 64 KB, so about 1.9 MB at 720p. At quality 100 a
detailed frame is around 850 KB and a noisy one can exceed the 920 KB that a
one-byte-per-pixel estimate would give, and the hardware encoder fails the
whole frame when its output does not fit. The buffer comes from PSRAM, so the
headroom is cheap.

The MJPEG loop also drops a failed frame rather than the connection, giving up
only after several consecutive failures — one awkward frame should not end a
stream.

## Auto exposure

The vendor IPA is unusable on ESP-IDF 6.x (see below), so this example does its
own metering:

1. Every frame, sample a sparse grid of the RGB565 buffer and compute mean
   Rec.601 luma. Sampling ~4000 pixels keeps this well under a millisecond even
   at 720p.
2. Every third frame, compare against the target and correct.
3. Adjust **exposure** first, since it costs no noise. Only once exposure is
   pinned at an end stop does it trade in **gain**.
4. Corrections are proportional, damped and capped at 25% per step, with a dead
   band, so the image settles instead of hunting.

Measured convergence from a dark start:

```
luma=29   exposure=1244  gain=26     <- dark, opening up
luma=100  exposure=1244  gain=46
luma=90   exposure=1108  gain=48
luma=101  exposure=347   gain=48     <- scene got brighter, stopping down
luma=103  exposure=364   gain=48     <- settled
```

This is what stops the image blowing out when you carry the board from indoors
to outdoors.

## Measured performance

Over Wi-Fi via the on-board ESP32-C6:

| Stream | Resolution | Frame rate | Bitrate |
|---|---|---|---|
| RTSP H.264 | 1920x1080 | ~15 fps | 4 Mbit/s |
| RTSP H.264 | 1280x720 | ~30 fps | 2.49 Mbit/s |
| RTSP H.264 | 800x800 | ~30 fps | 1.5 Mbit/s |
| RTSP H.264 | 640x480 | ~36 fps | 0.8 Mbit/s |
| HTTP MJPEG (quality 80) | 1280x720 | ~29.7 fps | ~1.3 MB/s (~10 Mbit/s) |

1080p is the only mode that does not hold 30 fps; the limit is encode plus
PSRAM bandwidth, not the network, since the stream is only 4 Mbit/s.

## How it works

1. The BSP brings up the shared I2C bus, which is reused as the sensor's SCCB
   bus (`init_sccb = false`) instead of creating a second master on the same pins.
2. `esp_video_init()` starts the MIPI-CSI device with the sensor reset on
   `BSP_CSI_PIN_RESET` (GPIO32, the `CSI_IO0` line on the camera connector).
3. Frames are captured from `/dev/video0` as YUV420 at 1280x720 through the
   standard V4L2 mmap flow (`REQBUFS` / `QBUF` / `DQBUF`).
4. For **RTSP**, each frame goes straight to the hardware H.264 encoder
   (`/dev/video11`, the esp_video M2M device). The input queue uses `USERPTR`,
   so the camera's own buffer reaches the encoder with no copy at all.
   `rtsp_server.c` then splits the access unit on Annex-B start codes and
   packetises each NAL as RTP.
5. For **MJPEG**, the PPA converts the frame to RGB565, the hardware JPEG
   encoder compresses it, and `app_httpd.c` sends the result as a
   `multipart/x-mixed-replace` stream.

Only one consumer runs at a time. Ownership is a token that increments on every
acquire, so a displaced consumer notices its token no longer matches and exits
its loop — no locks held across a frame.

## Known issue: ISP auto-exposure / auto-white-balance is disabled

`CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` is set to `n` here, which
means no automatic exposure, gain or white balance.

The reason is an upstream bug. `esp_ipa` (pulled in by the pipeline controller)
ships as a **prebuilt binary**, and the ESP-IDF 6.x build of it is compiled with
the RISC-V **Zba** extension:

```
lib/esp32p4/v6.0+/libesp_ipa.a         139 sh1add/sh2add/sh3add instructions
lib/esp32p4/v6.0+/libesp_ipa_newlib.a  139
lib/esp32p4/v5.5/libesp_ipa.a            0
```

The ESP32-P4 does not implement Zba (`-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`),
so the first `sh2add` executed inside `esp_ipa_pipeline_create()` raises:

```
Guru Meditation Error: Core 0 panic'ed (Illegal instruction)
MEPC : 0x40045906   MTVAL : 0x20fac7b3      <-- sh2add a5, s5, a5
```

The IDF 5.5 build of the same library is clean, so this only affects IDF 6.x.
Re-enable the pipeline controller once it is fixed upstream.

As a stand-in, this example implements its own auto-exposure loop (see above)
and exposes the sensor and ISP controls through the web UI.

White balance is still fixed rather than automatic; the defaults are 1.90x red
and 1.75x blue, adjustable live from the page. These matter: a Bayer sensor has
twice as many green photosites as red or blue, so without them the image has a
heavy green cast.

## Next steps

- Browsing previously saved snapshots over HTTP
- Timestamped filenames using the on-board RV-3028 RTC
- Automatic white balance using the ISP's AWB statistics
- Re-enable the ISP pipeline controller when `esp_ipa` is fixed
- Drop the private-header dependency in `app_sensor.c` if esp_video ever gains
  a supported way to enumerate and select sensor modes
