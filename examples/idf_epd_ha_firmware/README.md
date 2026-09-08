# Home Assistant e-paper firmware — epdInky ESP32-P4/C6 rev.2

A generic, protocol-agnostic firmware for this board: it joins Wi-Fi (or puts
up a captive portal if it isn't provisioned yet), then - once connected -
walks you through a second, network-reachable setup page for MQTT and the
dashboard if those aren't configured yet. Once fully set up it announces
itself to Home Assistant over MQTT discovery with a handful of diagnostic
sensors and a "refresh now" button, fetches a dashboard image from the URL
you configured, and shows it on the panel. Then it sleeps and repeats.

This firmware does **not** render anything itself. It has no dashboard
templates, no layout engine, no fonts beyond the plain-text status screens for
its own error states. Step 5 below is a plain `GET`; whatever PNG comes back
is what goes on the glass.

## Prerequisite: you need something to render the dashboard

This firmware fetches an already-rendered PNG; it does not talk to Home
Assistant's frontend or render Lovelace itself. You need a separate,
self-hosted service that:

- returns a PNG at (or scalable to) the panel's resolution — 1872×1404 for
  the default ED103TC2 panel, 2200×1650 for ED133UT2 (see the panel Kconfig
  choice below);
- optionally checks a Bearer token, if you want the URL to not be wide open.

A headless-browser screenshot tool pointed at a Home Assistant dashboard URL
(kiosk-mode, one browser tab, a scheduled screenshot) is the usual way to do
this — for example a small script driving a browser automation tool, or one
of the several open-source "HA dashboard to e-ink image" render services.
Setting one of those up is out of scope for this firmware; all it needs is a
URL that returns a PNG.

You will also want an MQTT broker already configured in Home Assistant
(**Settings → Devices & Services → MQTT**) so the discovery messages this
firmware publishes are picked up automatically.

## What happens on first boot: setup in two stages

Setup is split across two screens/servers, so that the only thing ever asked
for over the device's own open SoftAP is the one field that has to be: the
Wi-Fi network. Everything else - the MQTT broker, the dashboard URL, a Home
Assistant token - is collected afterwards, over the network the device just
joined.

### Stage 1 — Wi-Fi (SoftAP captive portal)

With no Wi-Fi network stored yet — a fresh flash, or one whose configuration
was just erased via the button's factory-reset gesture (held > 20 s) — the
device puts up an open SoftAP named `ha-epdinky-XXXXXX` (the last three bytes
of its Wi-Fi MAC) and shows a setup screen on the panel with that SSID (also
encoded as a `WIFI:` QR code, scannable by any phone camera to join directly)
and the portal URL, `http://4.3.2.1/`.

Connect a phone or laptop to that network (or scan the QR code), browse to
`http://4.3.2.1/`, and fill in the Wi-Fi network — pick from the scanned list
or type the SSID by hand, plus the password. Submitting saves just those two
fields to NVS and reboots into station mode.

### Stage 2 — MQTT and the dashboard (plain config server)

Once the device has joined that network and has a real DHCP address, if MQTT
or the dashboard URL are still unset it shows "Wi-Fi connected — open this
address to finish setup" with that address on the panel, and starts a plain
HTTP server there (no SoftAP, no captive-portal redirect tricks — it's an
ordinary page on your own network now).

Browse to the address shown and fill in:

- **MQTT broker host/port** — e.g. `mqtt.home.local`, port pre-filled from
  `CONFIG_HA_MQTT_DEFAULT_PORT` (1883 by default) the first time, or the
  stored value on a later visit; username/password if your broker needs
  them, blank otherwise;
- **Dashboard image URL** — e.g. `https://render.home.local/dashboard.png`;
- **Home Assistant long-lived access token** — sent as `Authorization: Bearer
  <token>` when fetching the image; leave blank if your render service needs
  no auth;
- **Refresh interval** — seconds between cycles, pre-filled from
  `CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S` (900 = 15 minutes by default) the
  first time, or the stored value on a later visit.

Submitting saves everything to NVS and reboots into the normal cycle below.
Nothing above is compiled in anywhere — there is no `ha_credentials.h` to
edit, deliberately: see "Security notes" below.

**Every field except the two secrets is pre-filled from what's already
stored** (`GET /current`), so visiting this page again — via the button's
reconfigure gesture below — only means retyping what you actually want to
change. The broker password and the Home Assistant token are the exception:
this page never sends an already-stored secret back to the browser to show
it, so those two fields always start blank with a placeholder noting one is
already set, and **leaving one blank on submit keeps the existing value** -
only typing a new one replaces it.

Either stage gives up and deep-sleeps for 15 minutes if nobody submits its
form — so an unattended device doesn't hold a radio and an HTTP server open
indefinitely — and picks back up where it left off (the same stage, the same
screen) on its next timed wake.

## The cycle

Every wake:

1. brings up the PMIC, panel, 4bpp framebuffer, RTC, fuel gauge and button;
2. joins Wi-Fi (or drops into the stage-1 portal above if no network is
   stored), then drops into the stage-2 config server instead if MQTT or the
   dashboard URL aren't set yet;
3. corrects the clock over SNTP once a day, writing the result back to the
   RV-3028 (seeded from the RTC at boot, so time is available immediately
   even before the first network round trip);
4. connects to the configured MQTT broker, publishes retained Home Assistant
   discovery configs and current state, listens briefly
   (`CONFIG_HA_MQTT_COMMAND_WINDOW_S`, 3 s by default) for a "refresh now"
   button press from Home Assistant, then disconnects;
5. `GET`s the dashboard image and shows it;
6. sleeps `refresh_s` seconds (idle or deep, per Kconfig) and repeats. **Idle
   builds keep a second, background MQTT connection up for the whole of that
   wait** (not just step 4's brief window) subscribed to the same
   refresh-button command topic, so pressing "Refresh Now" in Home Assistant
   ends the wait immediately - the same as a physical short button-press -
   instead of only being caught if you happen to press it during the few
   seconds step 4 is connected. Deep-sleep builds can't do this: the CPU is
   off for the whole wait, so a press there is only seen on the next
   scheduled wake, same as the physical button's own limitation below.

Steps 4 and 5 are independent: a broker that's down for a while costs this
cycle's telemetry, not the dashboard refresh, which is the reason a
battery-powered display exists in the first place. A Wi-Fi failure is the one
truly gating error — nothing past it can do anything useful without a
network — so it's the only arm that skips the rest of the cycle outright. Any
failure (Wi-Fi, MQTT connect, image fetch/decode) shortens the next sleep to
`HA_RETRY_INTERVAL_S` (60 s) rather than waiting out the full refresh
interval, and shows a plain status screen on the panel so a real fault is
visible without a serial console attached.

## Home Assistant entities

Every entity is retained-discovered under `homeassistant/<component>/<device
id>/<object_id>/config`, where the device id is `ha_epdinky_XXXXXX` (the last
three MAC bytes, same numbering as the SoftAP name) — stable across
re-provisioning, so moving the same board to a new Wi-Fi network or broker
doesn't create a duplicate device in Home Assistant. All entities share one
`device` block, so they group under a single device named "epdInky
Dashboard":

| Entity | Type | Notes |
|---|---|---|
| Battery Voltage | sensor, V | always published |
| Battery | sensor, % | only published when a battery is actually detected — see below |
| Battery Charging | binary_sensor | `device_class: battery_charging` |
| Wi-Fi Signal | sensor, dBm | `device_class: signal_strength` |
| Last Refresh | sensor, timestamp | time of the last cycle that got a fresh image onto the panel |
| Refresh Now | button | publishes to `ha_epdinky/<id>/refresh/set`; in idle builds ends the current wait immediately (see step 6 above), in deep-sleep builds only seen on the next scheduled wake |

The battery percentage is the STC3115 fuel gauge's own `soc_permille`
register — a real gas-gauge estimate, not something this firmware derives
from a voltage curve. There is no VBUS/USB-detect pin on this board, so
"is a battery even connected" comes from the gauge's own `BATFAIL` bit
(backed by a voltage floor); when neither reading indicates a battery, only
the voltage sensor is published rather than fabricating a percentage that
doesn't mean anything.

The device announces `online` on `ha_epdinky/<id>/availability` on connect,
but - unlike an early version of this firmware - does **not** announce
`offline` before its own ordinary disconnect at the end of each cycle: doing
so made every entity show "Unavailable" for all but a few seconds out of
every refresh interval, since this device is only reachable that briefly by
design. Instead, every sensor/binary_sensor's discovery config carries an
`expire_after` of roughly twice the refresh interval, so Home Assistant keeps
showing the last real reading until it's actually stale - i.e. until the
device has missed more than one check-in - rather than flapping to
"unavailable" and back every single cycle. A genuine crash or dropped
connection still shows up as unavailable immediately, via the
Last-Will-and-Testament on that same availability topic.

## The button: three gestures, and a limitation

`CONFIG_HA_WAIT_IDLE` (idle between cycles) polls the on-board button
(GPIO35, SW4) for three gestures, told apart by how long it's held:

| Hold | Gesture | Effect |
|---|---|---|
| < 5 s | refresh now | starts a cycle immediately |
| 5–20 s | reconfigure | reboots straight into the stage-2 config server, form pre-filled from what's already stored (Wi-Fi and every existing MQTT/dashboard setting are kept, nothing is erased) |
| > 20 s | factory reset | clears everything, including Wi-Fi, and reboots into the stage-1 captive portal |

The 5–20 s gesture is the one to use for changing broker or dashboard details
later without having to re-join Wi-Fi or retype settings you're not
changing: it drops the device into exactly the same stage-2 screen a freshly
Wi-Fi-provisioned device shows itself, except this time every field but the
two secrets (broker password, Home Assistant token) already shows its
current value - see "What happens on first boot" above for how those two are
handled.

**In `CONFIG_HA_WAIT_DEEP_SLEEP` builds neither button is immediate.** The
ESP32-P4 only has `ext0`/`ext1` wake on GPIO0–15; the physical button is on
GPIO35, and every low-numbered pin on rev.2 is already spoken for (GPIO0/1
are the 32 kHz crystal, GPIO2–17 the EPD data bus) — there is no board
arrangement that wakes it from a press. The same underlying problem rules out
an immediate Home Assistant "Refresh Now" too: the CPU is off for the whole
sleep, so there's nothing that could hold an MQTT connection open to catch a
push with. Deep-sleep builds only see either one on the device's *next*
scheduled wake. Press RESET for an immediate refresh instead, or build with
`CONFIG_HA_WAIT_IDLE` if either button mattering live is more important to
you than sleep current - idle builds keep an MQTT listener up for the whole
wait specifically so "Refresh Now" is immediate there (see step 6 above).

## Kconfig options

**Home Assistant e-paper firmware** (`idf.py menuconfig`):

- **What the device does between cycles** — `CONFIG_HA_WAIT_DEEP_SLEEP`
  (default) or `CONFIG_HA_WAIT_IDLE`. See the button limitation above before
  picking deep sleep on a bench where you'll want the button.
- **Which e-paper panel is physically connected** — `CONFIG_HA_PANEL_ED103TC2`
  (default, 10.3", 1872×1404) or `CONFIG_HA_PANEL_ED133UT2` (13.3",
  2200×1650). Reported resolution always
  matches the panel choice — there's no external server whose rendering
  assumptions this firmware has to keep a promise to, so point your render
  service at whichever size you pick here.
- **Default MQTT broker port** (`CONFIG_HA_MQTT_DEFAULT_PORT`, 1883) and
  **Default refresh interval** (`CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S`, 900)
  — pre-fill the stage-2 config server's form; the actual values used are
  whatever ends up in NVS.
- **How long to listen for a refresh-button press each cycle**
  (`CONFIG_HA_MQTT_COMMAND_WINDOW_S`, 3) — every second here is Wi-Fi and
  MQTT held open once a cycle, whether or not anyone presses the button that
  cycle, so it's worth keeping short.

## Security notes

- No real credentials, brokers or tokens are compiled in anywhere in this
  tree — everything above is collected once, at runtime, through the
  two-stage setup, and lives only in this device's NVS. Any example value in
  this README (`mqtt.home.local`, `<long-lived-access-token>`) is a
  placeholder, not a real one.
- **HTTPS certificates are verified by default.** `main/ha_http.c` attaches
  the mbedTLS certificate bundle (`esp_crt_bundle_attach`) to every `https://`
  request, which disables verification for one specific, named external server by
  deliberate decision. If your own render service or broker uses a
  self-signed certificate, don't turn the bundle off globally
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`); instead give `ha_http_get()` its own
  `esp_tls_cfg_t` with either that certificate pinned or
  `.skip_common_name_check`/insecure mode set for just that call.
- MQTT in this example connects over plain TCP only, to a broker on your own
  local network — the portal collects a host and port, not a certificate.
  If you need MQTT over TLS, `ha_mqtt.c`'s `esp_mqtt_client_config_t` is the
  place to add a `broker.address.transport = MQTT_TRANSPORT_OVER_SSL` and a
  certificate/bundle.
- Both stages' `/connect` handlers (and stage 1's `/scan`) bound every field
  to a fixed-size buffer via `strlcpy`/a clamped integer parse before it ever
  reaches NVS — an over-long or malformed submission is truncated or
  rejected, not something that can overflow anything.

## Build and flash

Requires ESP-IDF v6.0.2. On this machine, activate it with the leading dot —
the stock `export.sh` does not work here:

```sh
cd /home/dale/TestPlayground/aa_epdInky_bsp
. ./export-idf.sh
```

Then:

```sh
cd examples/ha-firmware
. ../../export-idf.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

The first build downloads two managed dependencies (`espressif/cjson` for
MQTT discovery/portal JSON, `espressif/mqtt` for the MQTT client — see
`main/idf_component.yml`) and takes a few minutes.

No credentials file to copy or edit — see "What happens on first boot" above.

## Files

| File | Role |
|---|---|
| `main/main.c` | the cycle: Wi-Fi → HA config gate → clock → MQTT → image fetch → display → sleep |
| `main/ha_config.h` | build-time firmware identity, panel geometry, NVS key names |
| `main/ha_http.c/.h` | `esp_http_client` GET with a Bearer header, PSRAM body out, verified TLS |
| `main/ha_mqtt.c/.h` | one wake's MQTT work: connect, discovery, state, brief command listen |
| `main/ha_image.c/.h` | PNG (+ 1-bpp BMP) decoder |
| `main/ha_display.c/.h` | the only panel owner: framebuffer, refresh, plain-text status screens |
| `main/qrcodegen.c/.h` | vendored QR encoder (Project Nayuki, MIT) for the stage-1 Wi-Fi setup screen |
| `main/ha_persist.c/.h` | the NVS store behind everything provisioned at runtime |
| `main/ha_time.c/.h` | RV-3028 at boot, SNTP once a day, written back |
| `main/ha_sleep.c/.h` | the wait between cycles, and what woke us |
| `main/ha_button.c/.h` | short-press ("refresh now") / 5–20 s ("reconfigure") / >20 s ("factory reset") |
| `main/ha_battery.c/.h` | STC3115: voltage, percent, charging, battery presence |
| `main/portal/ha_portal.c/.h` | stage 1 (SoftAP + captive portal, Wi-Fi only) and stage 2 (plain HTTP server, MQTT/dashboard) |
| `main/portal/ha_portal_wifi_page.h` | stage-1 HTML/JS: network scan, SSID/password |
| `main/portal/ha_portal_config_page.h` | stage-2 HTML/JS: MQTT/dashboard/token/refresh fields |
| `main/Kconfig.projbuild` | sleep mode, panel choice, MQTT/refresh defaults, command window |
| `sdkconfig.defaults` | board configuration — **the source of truth**, not `sdkconfig` |
| `partitions.csv` | 16 MB: nvs / phy / one factory app slot / spare storage (no OTA, no image cache) |

## What's deliberately not here

- **No renderer.** This firmware fetches a PNG; it has no idea what a
  Lovelace dashboard is. See "Prerequisite" above.
- **No OTA.** The partition table has one factory app slot. Reflash over USB
  to update.
- **No offline image cache.** Every cycle fetches fresh — there's no SD card
  dependency and nothing to keep in sync between wakes, at the cost of a
  network round trip every time even if the dashboard hasn't changed.
- **No MQTTS.** See "Security notes" above for where to add it if you need
  it.
