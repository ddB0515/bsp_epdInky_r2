# Home Assistant TFT dashboard — epdInky ESP32-P4 (TFT+touch variant)

An always-on, interactive Home Assistant dashboard for this board's
MIPI-DSI TFT panel with capacitive touch, built with vanilla LVGL 9.5.
Touching a tile calls a Home Assistant service immediately; every tile
updates live when its entity's state changes in Home Assistant, from any
source.

Unlike [`examples/idf_epd_ha_firmware`](../idf_epd_ha_firmware) (the e-paper
sibling of this firmware), this device does not sleep, does not fetch a
pre-rendered image, and talks to Home Assistant's native WebSocket API
directly rather than MQTT. It's mains-powered and stays on.

## Hardware

Same epdInky ESP32-P4/C6 mainboard, with the e-paper panel/PMIC replaced by
the D320C2403V1-MIPI panel already proven in
[`examples/idf_dsi_camera_preview`](../idf_dsi_camera_preview):

| | |
|---|---|
| Panel | D320C2403V1-MIPI, 3.2", 1024x768, MIPI-DSI, RGB888 |
| Driver | JD9168, 2 data lanes |
| Touch | GT967 over I2C at 0x5D (GT911-protocol-compatible) |
| Backlight | SGM37604A over I2C at 0x36 (no PWM/GPIO backlight on this board) |
| Adapter | The panel's own TCA6408 at 0x20 (mainboard's is 0x21) |

The panel plugs into FPC1 through an adapter board that gates its own power,
reset and backlight-enable through that TCA6408 — see
`main/ha_panel_jd9168.c` and `components/epdinky_p4_board/include/bsp/config.h`
for the exact sequencing, or
[`idf_dsi_camera_preview`'s README](../idf_dsi_camera_preview/README.md) for
the full bring-up story.

This firmware disables the e-paper PMIC (`tps65185`) and the battery fuel
gauge (`stc3115`) in the shared board-support package — neither is fitted on
this build — via `bsp_epdinky_config_t.enable`; no BSP code changes were
needed for that.

## Panel abstraction

`main/ha_panel.h` and `main/ha_touch.h` are hard interfaces every concrete
panel/touch driver implements — nothing else in this firmware (`ha_lvgl.c`,
`ha_dashboard.c`, `main.c`) ever references the JD9168 or GT911 directly. A
future different panel is a new `ha_panel_<name>.c` implementing five
functions (`ha_panel_init/get_handle/width/height/set_backlight`), selected
via the `HA_PANEL` Kconfig choice; only one physical panel is supported
today.

## Home Assistant integration

Talks to `ws://<host>:<port>/api/websocket` directly: authenticates with a
long-lived access token, subscribes to `state_changed`, fetches the current
state of every tracked entity once at connect, and calls services on a tap
or drag. See `main/ha_ws.c` for the protocol client (reconnects indefinitely
with backoff — Home Assistant restarts routinely, e.g. after an update) and
`main/ha_dashboard.c` for how that drives the LVGL tiles.

Tiles sit in a fixed `LV_LAYOUT_GRID` (not a wrapping flex flow), each with
a power-glyph icon that recolors green/grey with the entity's on/off state.
`HA_TILE_LIGHT`/`HA_TILE_SWITCH` tiles have a switch that calls
`homeassistant.toggle` on tap (domain-agnostic, no need to guess the current
state); `HA_TILE_LIGHT` tiles additionally get a brightness slider (shown
once the entity reports one — some lights aren't dimmable) that calls
`light.turn_on` with `brightness_pct` on release, not on every drag tick.
`HA_TILE_SENSOR` tiles are display-only.

### Editing the dashboard (do this before flashing a real installation)

**`main/ha_dashboard_config.h` ships with placeholder entity IDs.** Open it
and replace `HA_DASHBOARD_TILES` with your own entities — find real
`entity_id`s in Home Assistant under **Settings → Devices & Services →
Entities**, or **Developer Tools → States**. This is a compiled-in list for
v1, edited and reflashed per installation, not a runtime tile editor.

```c
static const ha_dashboard_entity_t HA_DASHBOARD_TILES[] = {
    { "light.living_room", "Living Room", HA_TILE_LIGHT,  NULL  },
    { "sensor.outside_temp", "Outside",   HA_TILE_SENSOR, "°C" },
};
```

See "Home Assistant integration" above for what each tile kind does.

## Setup

Two provisioning stages, same shape as the e-paper firmware, shown on this
panel via `main/ha_setup_screen.c` instead of plain e-paper text — this
panel draws its own `WIFI:` QR code with LVGL's built-in QR widget rather
than vendoring a QR encoder.

1. **Wi-Fi (SoftAP captive portal)** — on first boot (or after a factory
   reset), the device puts up an open SoftAP named `ha-tft-XXXXXX` and shows
   its SSID, a scannable QR code, and the portal URL. Join it and browse to
   `http://4.3.2.1/` to submit your Wi-Fi network.
2. **Home Assistant (plain config server)** — once connected, if no Home
   Assistant host/token is stored, the device shows "Wi-Fi connected — open
   this address" and starts an ordinary HTTP server on its new station IP.
   Submit your Home Assistant address (e.g. `homeassistant.local`), port
   (default from Kconfig, usually `8123`), and a
   [long-lived access token](https://www.home-assistant.io/docs/authentication/#your-account-profile)
   (Settings → your profile → Security).

Unlike the e-paper firmware, neither stage times out or deep-sleeps: this
device is mains-powered, so there's no battery reason to tear a setup screen
down if nobody's configuring it yet. It just waits, fully responsive.

## The button

Same three gestures as the e-paper firmware (`main/ha_button.c`, unchanged):

| Hold | Gesture | Effect |
|---|---|---|
| < 5 s | wake backlight | brings the backlight to full brightness immediately, without waiting for a touch |
| 5–20 s | reconfigure | reboots into the Home Assistant setup screen, Wi-Fi kept |
| > 20 s | factory reset | erases everything, including Wi-Fi, and reboots into the SoftAP portal |

There's no "refresh now" gesture here — the dashboard is always live over
its WebSocket connection, there's no cycle to end early.

## Kconfig options

**Home Assistant TFT dashboard firmware** (`idf.py menuconfig`):

- **Which display panel is physically connected** — `HA_PANEL` choice, one
  option today.
- **Backlight: three stages** — full brightness
  (`HA_DISPLAY_BRIGHTNESS_PERCENT`, 80) until `HA_DISPLAY_IDLE_TIMEOUT_S`
  (120s) of no touch, then dimmed (`HA_DISPLAY_DIM_PERCENT`, 15) for a
  further `HA_DISPLAY_OFF_DELAY_S` (60s), then off entirely. Backlight is
  I2C (SGM37604A), not PWM — LVGL's own input-activity tracking
  (`lv_display_get_inactive_time()`) drives all of this, so any touch
  restores full brightness immediately, direct from off, with no extra
  bookkeeping and no need to pass back through the dimmed stage first.
- **Default Home Assistant WebSocket port** — `HA_WS_DEFAULT_PORT` (8123),
  pre-fills the Stage 2 form.
- **Reconnect backoff min/max** — `HA_WS_RECONNECT_MIN_MS` (1000) /
  `HA_WS_RECONNECT_MAX_MS` (30000), doubling with jitter between them.

## Security notes

- No real credentials, hosts or tokens are compiled in anywhere in this tree
  — everything above is collected once, at runtime, through the two-stage
  setup, and lives only in this device's NVS.
- This firmware connects to Home Assistant over plain `ws://`, not `wss://`,
  to a host on your own local network — the portal collects a host and
  port, not a certificate. If you need TLS, `ha_ws.c`'s
  `esp_websocket_client_config_t` is the place to switch to `wss://` and add
  a certificate/bundle.
- The Stage 1/2 `/connect` handlers (and Stage 1's `/scan`) bound every field
  to a fixed-size buffer via `strlcpy`/a clamped integer parse before it
  ever reaches NVS.

## Build and flash

Requires ESP-IDF v6.0.2. On this machine, activate it with the leading dot:

```sh
cd /home/dale/TestPlayground/aa_epdInky_bsp
. ./export-idf.sh
```

Then:

```sh
cd examples/idf_ha_firmware
. ../../export-idf.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

The first build downloads several managed dependencies (LVGL 9.5, the GT911
touch driver, cJSON, the WebSocket client — see `main/idf_component.yml`)
and takes a few minutes.

**Edit `main/ha_dashboard_config.h` with your real entity IDs before
flashing a real installation** — see "Editing the dashboard" above.

## Files

| File | Role |
|---|---|
| `main/main.c` | board init → provisioning gates → dashboard/WebSocket start → button-poll loop (no sleep) |
| `main/ha_config.h` | build-time firmware identity, NVS key names, buffer sizes |
| `main/ha_panel.h` / `ha_panel_jd9168.c` | the panel abstraction and today's one concrete implementation |
| `main/ha_touch.h` / `ha_touch_gt911.c` | the touch abstraction and today's one concrete implementation |
| `main/ha_lvgl.c/.h` | LVGL bring-up: tick, draw buffers, flush callback, the LVGL task and lock |
| `main/ha_setup_screen.c/.h` | the two provisioning-stage screens (SoftAP QR code, "browse to...") |
| `main/ha_ws.c/.h` | the Home Assistant WebSocket client: auth, subscribe, get_states, call_service, reconnect |
| `main/ha_dashboard.c/.h` | LVGL tiles bound to entities; tap → call_service, state change → tile update |
| `main/ha_dashboard_config.h` | **edit this** — the compiled-in v1 tile list |
| `main/ha_persist.c/.h` | the NVS store behind everything provisioned at runtime |
| `main/ha_button.c/.h` | short ("wake backlight") / medium ("reconfigure") / long ("factory reset") gestures |
| `main/portal/ha_portal.c/.h` | Stage 1 (SoftAP + captive portal, Wi-Fi only) and Stage 2 (plain HTTP server, Home Assistant) |
| `main/portal/ha_portal_wifi_page.h` | Stage 1 HTML/JS: network scan, SSID/password |
| `main/portal/ha_portal_config_page.h` | Stage 2 HTML/JS: Home Assistant host/port/token fields |
| `main/Kconfig.projbuild` | panel choice, backlight/idle-dim, WebSocket port/reconnect defaults |
| `sdkconfig.defaults` | board configuration — **the source of truth**, not `sdkconfig` |
| `partitions.csv` | 16 MB: nvs / phy / one factory app slot / spare storage (no OTA) |

## What's deliberately not here

- **No runtime dashboard editor.** The tile list is compiled in
  (`ha_dashboard_config.h`) and reflashed per installation. A portal-driven
  or JSON tile editor is a natural v2, not part of this pass.
- **No OTA.** The partition table has one factory app slot. Reflash over USB
  to update.
- **No MQTT.** This firmware talks to Home Assistant's WebSocket API
  directly; see `examples/idf_epd_ha_firmware` for this repo's MQTT-based
  example instead.
- **No `wss://`.** See "Security notes" above for where to add it.
