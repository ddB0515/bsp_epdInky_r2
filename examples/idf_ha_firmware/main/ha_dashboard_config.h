/*
 * v1 dashboard tile list — compiled in, edited and reflashed per
 * installation, not provisioned over the network. This is a deliberate v1
 * scope cut, not an oversight: see the project plan for why. A portal-driven
 * or JSON tile editor is a natural v2 if wanted later.
 *
 * ────────────────────────────────────────────────────────────────────────
 *  EDIT THIS FILE before flashing a real installation. The entity_ids below
 *  are placeholder examples, not real Home Assistant entities - replace them
 *  with entity_ids from your own Home Assistant instance (Settings ->
 *  Devices & Services -> Entities, or Developer Tools -> States).
 * ────────────────────────────────────────────────────────────────────────
 */
#pragma once

typedef enum {
    HA_TILE_LIGHT,   /* tappable, calls homeassistant.toggle              */
    HA_TILE_SWITCH,  /* tappable, calls homeassistant.toggle              */
    HA_TILE_SENSOR,  /* display-only, shows state + unit, no tap action   */
} ha_tile_kind_t;

typedef struct {
    const char     *entity_id;
    const char     *label;   /* shown on the tile; kept short, this is a touch UI */
    ha_tile_kind_t  kind;
    const char     *unit;    /* sensors only, e.g. "\xC2\xB0" "C"; NULL otherwise */
} ha_dashboard_entity_t;

static const ha_dashboard_entity_t HA_DASHBOARD_TILES[] = {
    { "light.worklights",              "Work Light",       HA_TILE_LIGHT,  NULL   },
    { "light.workfront",              "Work Light Front",       HA_TILE_LIGHT,  NULL   },
    { "light.workback",              "Work Light Back",       HA_TILE_LIGHT,  NULL   },
    { "sensor.living_room_temperature", "Living Room Temp",  HA_TILE_SENSOR, "\xC2\xB0" "C" },
};

#define HA_DASHBOARD_TILE_COUNT (sizeof(HA_DASHBOARD_TILES) / sizeof(HA_DASHBOARD_TILES[0]))
