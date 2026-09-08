# TRMNL firmware — epdInky ESP32-P4/C6 rev.2

A native ESP-IDF port of [usetrmnl/trmnl-firmware][upstream] for this board.
No Arduino framework, no FastEPD, no bb_epaper: the panel is driven by this
repository's own `components/epd`, and every upstream dependency is replaced by
an IDF component or an in-tree one. 

[upstream]: https://github.com/usetrmnl/trmnl-firmware

## Current state: phase 3 — the cycle, with a real sleep

**This build is a working TRMNL device.** It fetches your frames and keeps
fetching them on the server's schedule, for as long as it is powered.

Once, at boot:

1. brings up the PMIC, the panel and a 1872×1404 4bpp framebuffer, the I/O
   expander, the RTC and the button,
2. seeds the clock from the RV-3028 and mounts the micro-SD card if one is
   fitted.

Then, every cycle:

3. joins Wi-Fi through the BSP (esp-hosted → on-board ESP32-C6),
4. corrects the clock over SNTP — once a day, not once a cycle — and writes the
   result back to the RTC,
5. registers with `GET /api/setup` if it has no stored key, keyed by the station
   MAC,
6. requests a frame with `GET /api/display`, advertising `Model: x` and
   `Width: 1872` / `Height: 1404`,
7. shows that frame: straight from the SD cache if it is already there, skipping
   the refresh altogether if it is the frame already on the glass, downloading
   and caching it otherwise,
8. sleeps `refresh_rate` seconds and repeats.

Every failure sets a back-off interval instead of giving up, so a device that
cannot reach the network or the server keeps retrying rather than needing a
reset. The first failure of a run also draws a status screen, so the device says
what went wrong without a serial console attached; later retries only log,
because a four-second panel flash every fifteen seconds would be worse than the
fault it is reporting. Every status screen carries the TRMNL pinwheel mark
(`main/trmnl_logo.h`), including the "nothing to show yet" screen a freshly
registered device with no plugin assigned yet will see — that state is
reported as informational, not an error.

`Model: x` is upstream's TRMNL X: a parallel-bus e-ink device with exactly this
board's 1872×1404 10.3" panel (`device_list[]` in `src/display.cpp`). Claiming
it gets server-side rendering already tuned for this geometry rather than an
800×480 image upscaled 2.3×.

What is *not* here yet: measuring the actual sleep current (both wait modes
exist specifically so that A/B can be run - see below) — still the largest
open question in the project. OTA (the firmware `update_firmware` arm) is
deferred by decision rather than missing: `update_firmware`/`firmware_url`
stay parsed and logged, and the partition table already reserves the space
for it regardless, so it can land later without a repartition. Everything
else in upstream's block scheme - Wi-Fi provisioning, registration,
`special_function` dispatch, `reset_firmware`, and a real battery reading
over the STC3115 - is implemented.

## The wait between cycles

Two builds of the same firmware, chosen in `menuconfig` under
**TRMNL firmware → What the device does between cycles**. They exist as a pair
so this board's sleep current can be measured by flashing one and then the
other, which is the number that decides whether the server's 15-minute duty
cycle is achievable on a battery at all.

**That measurement is deferred on this board's chip revision (1.0/1.3):**
these ESP32-P4 silicon revisions have documented deep-sleep issues, fixed
from revision v3.0 onward. Running the A/B on this hardware would measure
the erratum, not this port's power behaviour, so it waits for a
fixed-revision board. Idle is the
practical default in the meantime; deep sleep still builds and runs (both
modes are exercised by CI-equivalent local builds either way), it's just not
where the answer to "is this viable on a battery" is going to come from
right now.

## Battery

`main/trmnl_battery.c` reads the STC3115 fuel gauge once per cycle: voltage,
whether it's charging, and whether a battery is connected at all. That last
one has no dedicated signal to read — there's no VBUS/USB-detect pin
anywhere on this board's schematic, so a bench unit running on USB alone
looks the same as a battery-powered one to every GPIO. What the gauge does
have is BATFAIL: a status bit meaning UVLO under 2.6 V, which upstream's own
driver comment already names "battery removal detected". A voltage floor
backs it up in case BATFAIL is ever wrong on a given read, but they're two
readings of the same condition, not two different thresholds.

With no battery, `Battery-Voltage` falls back to the same placeholder this
port sent before the gauge was wired up (`4.10`), so a USB-only bench unit
doesn't get reported to the server as critically low. `Battery-Charging` is
only sent when a battery is actually present.

| | `TRMNL_WAIT_DEEP_SLEEP` (default) | `TRMNL_WAIT_IDLE` |
|---|---|---|
| between cycles | `esp_deep_sleep_start()`, timer wake | `vTaskDelay()`, radio associated |
| button | **does nothing** | starts a cycle immediately |
| each cycle | a fresh boot, ~2 s of start-up | continues in the same task |
| immediate refresh | press RESET | press the button |

Before sleeping the device unmounts the SD card and cuts its power, tears down
Wi-Fi so the C6 stops listening, and puts the TPS65185 into its `WAKEUP`-off
sleep state. The panel rails are already down — `trmnl_display_flush()` drops
them after every refresh.

**Deep sleep costs the button, permanently.** The P4 has 16 RTC-capable GPIOs
(0–15) and only those can drive an `ext0`/`ext1` wake. On rev.2 the button is
GPIO35, GPIO0/1 are the 32 kHz crystal and GPIO2–17 are the EPD data bus, so
there is no pin to move it to. Nothing polls it while the CPU is off. This is a
board fact, not a firmware limitation, and it is why the idle mode is kept
rather than deleted.

### What survives a sleep

Deep sleep resets the CPU, so the cycle's own state has to be told where to
live. Three lifetimes, in `main.c`:

| Where | What | Survives |
|---|---|---|
| plain statics | MAC, API key, friendly ID | one boot — re-read from NVS each time anyway |
| `RTC_DATA_ATTR` | retry counters, `needs_clean`, `prev_wake_time`, `image_cached` | a sleep; reset by a power cycle |
| NVS | refresh interval, credentials, the frame on the glass | everything |

The retry counters are in RTC fast memory rather than NVS on purpose: they
change on every failure, and a device offline for a day would otherwise write
the flash a hundred times to remember a number that stops mattering the moment
the network comes back. RTC fast memory is safe to rely on here because the P4
defines `SOC_RTC_FAST_MEM_SUPPORTED` and has no `SOC_PM_SUPPORT_RTC_FAST_MEM_PD`
— there is no way to power it down.

Because the name of the displayed frame is in NVS, an unchanged frame still
costs one `stat()` and no refresh after a sleep. That is the whole point of the
cache.

### Measuring

```sh
idf.py menuconfig     # TRMNL firmware -> What the device does between cycles
idf.py build flash
```

Flash each mode in turn and meter the board between cycles. The expected
complication is the ESP32-C6: its `EN` line is pulled up, so it does not
necessarily lose power when the P4 sleeps, and the saving may be well short of
upstream's. `bsp_wifi_deinit()` before sleeping is the only lever this firmware
has over it.

### The SD card is optional

The image cache lives on the micro-SD card. There is no card-detect line on this
board, so an absent or unformatted card simply shows up as a mount failure at
boot, logged as a warning:

```
W (…) trmnl_cache: no SD card (ESP_ERR_NOT_FOUND) - running uncached, every cycle will download and do a full refresh
```

That is a normal state, not a fault. With a card, an unchanged frame costs one
`stat()`; without one, every cycle is a download and a full GC16 refresh.

## Build and run

Requires ESP-IDF v6.0.2. On this machine, activate it with the leading dot —
the stock `export.sh` does not work here:

```sh
cd /home/dale/TestPlayground/aa_epdInky_bsp
. ./export-idf.sh
```

Then:

```sh
cd examples/trmnl-firmware
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 flash monitor
```

No credentials file is required. Wi-Fi lives entirely in NVS — there is no
compiled-in fallback — so a device with nothing provisioned yet (a fresh
flash, or one just cleared via `ClearWifi`: hold the button) boots straight
into a SoftAP, `TRMNL-XXXXXX`, open, and puts up a setup screen matching
upstream's own: the SSID and firmware version, join instructions, and a QR
code (`trmnl_display_show_wifi_setup()`) — scanning it offers to join the
SoftAP directly rather than typing the SSID in by hand, on any phone whose
camera recognises a `WIFI:` payload. Either way, `http://4.3.2.1/` is the
captive portal that actually collects real credentials; submitting the form
saves them to NVS and reboots into station mode.

`ClearWifi` is this device's one user-facing "factory reset": it erases the
registration along with the network, so the reboot that follows setup always
calls `/api/setup` fresh rather than silently resuming whatever device
identity happened to still be in NVS. A device that was never claimed on
trmnl.app will show the "sign up with this Friendly ID" screen again as part
of that.

`main/trmnl_credentials.h` (copy `trmnl_credentials.h.example`,
`.gitignore`d) is optional and only needed to point at a self-hosted TRMNL
server — see below.

The device must be registered on a trmnl.app account. On first boot the firmware
calls `/api/setup` with its MAC; the returned `api_key` and `friendly_id` are
written to NVS under upstream's own key names, so subsequent boots skip
registration. To force re-registration, erase NVS — this also erases the
Wi-Fi network, so the next boot re-provisions through the portal too:

```sh
idf.py -p /dev/ttyACM0 erase-flash
```

To point at a self-hosted TRMNL server, define `TRMNL_API_BASE_URL_OVERRIDE` in
`trmnl_credentials.h`.

### Reading the result

A good first cycle ends like this:

```
I (…) trmnl: TRMNL firmware 1.8.16, model "x", panel 1872x1404
I (…) trmnl: between cycles: deep sleep, timer wake only (the button does nothing)
I (…) trmnl_display: panel ready: 1872x1404
I (…) trmnl_time: clock seeded from the RTC: 2026-08-31 09:14:07 UTC
I (…) trmnl_cache: cache ready at /sdcard/trmnl
I (…) trmnl: === cycle: 2026-08-31 09:14:07 UTC, woken by powercycle ===
I (…) trmnl: Connected: IP …, MAC …, RSSI -52 dBm
I (…) trmnl: Using stored credentials for device 7ZXKQJ
I (…) trmnl: --- requesting a frame ---
I (…) trmnl: refresh_rate     : 900 s
I (…) trmnl: --- fetching image ---
I (…) trmnl: HTTP 200, Content-Type image/png, 14074 bytes
I (…) trmnl_cache: cached plugin-abc123.png (14074 bytes)
I (…) trmnl: format: PNG (decoded in-tree over the boot ROM's inflate)
I (…) trmnl_image: PNG 1872x1404, 4 bpp, colour type 0 -> 1315548 bytes of scanlines
I (…) trmnl: frame is on the panel
I (…) trmnl_sleep: sleeping 900 s (the button will not wake it; press RESET)
```

`woken by powercycle` is the first cycle after a reset; later ones say `timer`,
and in an idle build a cycle started by the button says `button`. The spellings
are upstream's `wakeupReasonMap` (`lib/trmnl/src/logging_parsers.cpp`) — the
server has seen these exact strings from every TRMNL device.

The cheap case — the server offering the same frame again — is one line and no
refresh at all:

```
I (…) trmnl: plugin-abc123.png is already on the panel; no refresh
```

A refresh that does happen takes a few seconds: an INIT pass, then GC16, plus
four clean cycles on a cold boot or after a status screen. The clean flashes
black and white before the image appears — that is deliberate, and the header
comment in `trmnl_display.h` says why.

## Files

| File | Role |
|---|---|
| `main/main.c` | the cycle: connect → clock → setup → `/api/display` → frame → wait |
| `main/trmnl_config.h` | protocol constants ported from upstream `include/config.h` |
| `main/trmnl_http.c/.h` | `esp_http_client` wrapper: GET and POST, headers in, PSRAM body out |
| `main/trmnl_api.c/.h` | request headers, cJSON response parsing, `/api/log` serialisation |
| `main/trmnl_image.c/.h` | PNG (plugin frames) + 1-bpp BMP (system screens) decoders |
| `main/qrcodegen.c/.h` | vendored, de-LVGL'd QR encoder for the Wi-Fi setup screen |
| `main/trmnl_logo.h` | the TRMNL pinwheel mark, as two 1-bpp bitmaps |
| `main/trmnl_display.c/.h` | the only panel owner: framebuffer, refresh, status screens |
| `main/trmnl_persist.c/.h` | upstream's `Persistence` interface over one NVS namespace |
| `main/trmnl_refresh.c/.h` | the `refresh_interval` back-off ladder, with upstream's constants |
| `main/trmnl_cache.c/.h` | the image cache on the micro-SD card |
| `main/trmnl_time.c/.h` | RV-3028 at boot, SNTP once a day, written back |
| `main/trmnl_sleep.c/.h` | the wait between cycles, and what woke us |
| `main/trmnl_button.c/.h` | short/double/long-press out of one GPIO |
| `main/trmnl_battery.c/.h` | STC3115: voltage, charging, battery presence |
| `main/portal/` | SoftAP + captive portal: Wi-Fi provisioning when there's none stored |
| `main/Kconfig.projbuild` | deep-sleep/idle and which panel is connected |
| `sdkconfig.defaults` | board configuration — **the source of truth**, not `sdkconfig` |
| `partitions.csv` | 16 MB: nvs / otadata / phy / 2×3 MB OTA / ~9.9 MB spare storage |

## Notes on the configuration

**TLS certificates are not verified.** `CONFIG_ESP_TLS_INSECURE` and
`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` are both set, which reproduces
upstream's `WiFiClientSecure::setInsecure()`. This is a deliberate v1 decision.
`sdkconfig.defaults` documents exactly what to change to switch to the mbedTLS
certificate bundle, which costs ~64 KB of flash and actually authenticates
trmnl.app.

Not verifying the certificate is not the same as not naming the server.
`esp_http_client_config_t::skip_cert_common_name_check` must stay unset: IDF
implements it as `mbedtls_ssl_set_hostname(ssl, NULL)`, which also suppresses
SNI, and trmnl.app's frontend answers a nameless handshake with a fatal alert
(`mbedtls_ssl_handshake returned -0x7780`).

**Bluetooth is off.** The TRMNL firmware has no use for it, and leaving the
NimBLE host out saves about 100 KB. The esp-hosted Wi-Fi path is unaffected.

**The console is USB Serial/JTAG.** UART0's default pins on the P4 are GPIO37
and GPIO38 — the TPS65185 `WAKEUP` and `INT` lines on this board. With a UART
console every log line would toggle the PMIC wake input.

**Deep sleep is timer-only.** The P4's `SOC_RTCIO_PIN_COUNT` is 16, so only
GPIO0–15 can drive `ext0`/`ext1` wake. On rev.2 the button is GPIO35, the
expander interrupt is GPIO34, GPIO0/1 are the 32 kHz crystal and GPIO2–17 are
the EPD data bus — no pin is free. See "The wait between cycles" above.

## The image path

Plugin content (a claimed device's `/api/display` frames) is **PNG**,
1872×1404, 4 bits per sample, non-interlaced, every row unfiltered. Two
variants have been observed:

| Screen | Colour type | Palette |
|---|---|---|
| system screens served via `/api/display` (unclaimed device, empty state) | 3 — indexed | 16-entry linear grey ramp, white→black |
| plugin renders (claimed device) | 0 — greyscale | none |

Both pack two 4-bit samples per byte with the leftmost pixel in the high nibble
and a 936-byte row stride, which is `epd_fb`'s packing exactly. The greyscale
variant needs no conversion at all — a decoded scanline *is* a framebuffer row,
so `row_to_grey4()` is a `memcpy`. The indexed variant runs white→black where
`epd_fb` runs black→white, so it goes through a 256-entry lookup that resolves
to `nibble ^ 0xF` for this particular palette while still handling any other.

16 grey levels means `EPD_WAVEFORM_GC16` after an INIT pass, not the DU path a
bilevel BMP would take.

`/api/setup`'s own `image_url` — the "sign up with this Friendly ID" screen a
never-claimed device gets — is different: **BMP**, uncompressed 1-bpp,
`https://trmnl.com/images/setup/setup-logo.bmp`. `trmnl_bmp_render()` decodes
it (BITMAPINFOHEADER, a 2-entry palette read from the file rather than
assumed, either row order) straight into the 1-bpp blit primitive added in
phase 1 - the one case that primitive was always meant for, even though the
first spike never saw it exercised.

Inflate comes from the P4 boot ROM (`tinfl_decompress`), so **no `libpng`, no
`zlib`, no G5 decoder and no hardware JPEG** — `trmnl_image.c` is a chunk walk,
an unfilter pass and a row packer, and costs nothing in dependencies.

It implements baseline PNG generally rather than assuming the shapes above:
colour types 0/2/3/4/6, bit depths 1/2/4/8/16, all five row filters, and
multi-`IDAT` streams. Interlaced images are rejected. Verification, host-side
against the real payloads and a synthetic matrix:

- both captured samples decode **pixel-exact** against a Pillow reference —
  0 mismatches out of 2,628,288 pixels;
- 15 colour-type × bit-depth combinations × filters 0–4 × `IDAT` splits 1/3/9 —
  all 225 pass (an earlier multi-`IDAT` bug was found this way);
- 522 truncated and bit-flipped inputs under ASan/UBSan — no crashes, no
  sanitizer reports.

## A panel trmnl.app doesn't support

**TRMNL firmware → Which e-paper panel is physically connected**
(`menuconfig`) picks `ED103TC2` (default, 1872×1404, what trmnl.app itself
renders for) or `ED133UT2` (13.3", 2200×1650 — not a size the server knows
about). Both are already in `components/epd`'s panel catalogue and both use
a 16-bit bus, so this is a `menuconfig` change, not a rewiring job.

`TRMNL_DISPLAY_WIDTH`/`HEIGHT` (`trmnl_config.h`) do not follow this choice —
the server only ever gets told 1872×1404, since that's the only way to keep
it sending real content instead of an error. Every frame that arrives under
`ED133UT2` is then smaller than the panel in both dimensions, which is
exactly the case `trmnl_image.c`'s decoders already handle without knowing
or caring what panel is attached: neither assumes decoded size equals
framebuffer size, both fall back to `epd_fb_blit_fit_rot()` /
`epd_fb_blit_1bpp_fit()` (aspect-preserving, letterboxed, upscale via pixel
repeat) whenever it doesn't match. Adding `ED133UT2` needed no new scaling
code — the fit path was already exercised by locally-rendered status screens
on size mismatches, just not yet by a real server frame on this panel.

## Reporting failures: `/api/log`

The first failure of a run (`report_failure()` in `main.c` - same "first
only" gate the status screen uses, so a long outage produces one log entry,
not one per retry) is POSTed to `/api/log`, whenever there's a network to
send it over. `trmnl_serialize_log_entry()` (`trmnl_api.c`) reproduces
upstream's `serialize_log.cpp` field-for-field - checked against upstream's
own test fixture host-side before this ever ran on a device, the same way
the PNG decoder was verified against real captured payloads.

That check caught a real bug: `battery_voltage` is a `float`, and
`cJSON_AddNumberToObject()` takes a `double` - widening one into the other
naively carries the float's binary imprecision along (`4.2f` prints as
`4.1999998092651367`, not `4.2`). `round2f()` fixes it the same way the
`Battery-Voltage` *header* a few lines above already does: format to two
decimal places as a string first, then parse that back into a double, rather
than trust the double you get from a bare cast.

Sent immediately, one entry per failure, best-effort — not upstream's
persistent store-and-batch queue (`stored_logs.cpp`), which keeps failed
submissions in NVS and sends everything gathered since in one batch once
connectivity returns. A submission that fails here is simply lost. That's a
real gap, not a simplification with no cost.

**Testing it:** `submit_log()` only fires from `report_failure()`, which only
fires from a real failure - awkward to trigger on demand without either
breaking a working device or waiting for the server to misbehave. **TRMNL
firmware → Send a test /api/log entry every cycle (debug)** (`menuconfig`,
`CONFIG_TRMNL_LOG_TEST_ON_BOOT`) sends one harmless test entry to the real
endpoint right after Wi-Fi and registration succeed each cycle, so the whole
path can be checked against a real account without engineering a fake
outage. Off by default, and meant to be turned back off once confirmed - it
POSTs to the real server every single cycle for as long as it's on.

## MISC

The immediate gap is measuring the actual sleep current — the number
`trmnl_battery.c`'s readings are for: it's what turns a duty cycle into a
runtime figure the device can report, and it's still the largest open
question in the project. OTA is deferred by decision rather than missing
(`update_firmware` stays parsed and logged; the partition table already
reserves the space for it regardless). And OTA is NOT SUPPORTED for now.
