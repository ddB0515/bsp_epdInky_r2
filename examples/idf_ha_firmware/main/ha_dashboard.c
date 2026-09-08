#include "ha_dashboard.h"

#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "ha_dashboard_config.h"
#include "ha_lvgl.h"
#include "ha_panel.h"
#include "ha_ws.h"

static const char *TAG = "ha_dashboard";

#define HA_TILE_W 300
#define HA_TILE_H 210
#define HA_TILE_GAP 16

/* Fixed column count; rows grow with the tile list. Both descriptor arrays
 * are filled once in build_ui() and must stay alive for as long as the grid
 * object exists (LVGL keeps the pointer, not a copy) - static, not stack,
 * for exactly that reason. */
#define HA_GRID_COLS 3
#define HA_GRID_ROWS ((HA_DASHBOARD_TILE_COUNT + HA_GRID_COLS - 1) / HA_GRID_COLS)

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;              /* NULL for HA_TILE_SENSOR - nothing to actuate */
    lv_obj_t *title_lbl;
    lv_obj_t *value_lbl;
    lv_obj_t *sw;                /* NULL for HA_TILE_SENSOR - nothing to tap */
    lv_obj_t *brightness_slider; /* HA_TILE_LIGHT only, hidden until a brightness is known */
    bool      slider_editing;    /* true while the user has a finger on the slider -
                                   * state_cb/reconcile must not fight the drag */
} ha_dashboard_tile_t;

static ha_dashboard_tile_t s_tiles[HA_DASHBOARD_TILE_COUNT];
static lv_obj_t           *s_status_banner;
static lv_obj_t           *s_status_label;

/* ===========================================================================
 * Helpers
 * ========================================================================= */

static int find_tile_index(const char *entity_id)
{
    for (size_t i = 0; i < HA_DASHBOARD_TILE_COUNT; i++) {
        if (strcmp(HA_DASHBOARD_TILES[i].entity_id, entity_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void apply_state_to_tile(size_t idx, const ha_ws_entity_state_t *st)
{
    const ha_dashboard_entity_t *ent  = &HA_DASHBOARD_TILES[idx];
    ha_dashboard_tile_t          *tile = &s_tiles[idx];

    if (!st->have_state) {
        lv_label_set_text(tile->value_lbl, "...");
        lv_obj_set_style_text_color(tile->value_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
        if (tile->icon) {
            lv_obj_set_style_text_color(tile->icon, lv_palette_main(LV_PALETTE_GREY), 0);
        }
        return;
    }

    bool on = (strcmp(st->state, "on") == 0);

    if (ent->kind == HA_TILE_SENSOR) {
        if (ent->unit) {
            lv_label_set_text_fmt(tile->value_lbl, "%s %s", st->state, ent->unit);
        } else {
            lv_label_set_text(tile->value_lbl, st->state);
        }
        lv_obj_set_style_text_color(tile->value_lbl, lv_color_white(), 0);
        return;
    }

    lv_color_t state_color = on ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_GREY);

    lv_label_set_text(tile->value_lbl, on ? "On" : "Off");
    lv_obj_set_style_text_color(tile->value_lbl, state_color, 0);
    if (tile->icon) {
        lv_obj_set_style_text_color(tile->icon, state_color, 0);
    }

    if (tile->sw) {
        if (on) {
            lv_obj_add_state(tile->sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(tile->sw, LV_STATE_CHECKED);
        }
    }

    /* Skip while the user has a finger on it - state_cb/reconcile firing
     * mid-drag would fight every touch move with a stale server value. */
    if (tile->brightness_slider && !tile->slider_editing) {
        if (st->brightness_pct >= 0) {
            lv_obj_remove_flag(tile->brightness_slider, LV_OBJ_FLAG_HIDDEN);
            lv_slider_set_value(tile->brightness_slider, st->brightness_pct, LV_ANIM_OFF);
        } else {
            /* This light doesn't report a brightness (non-dimmable, or the
             * attribute just hasn't arrived yet) - nothing sensible to show
             * or drag. */
            lv_obj_add_flag(tile->brightness_slider, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ===========================================================================
 * ha_ws.c callbacks - fire on ha_ws_task's context, never the LVGL task
 * ========================================================================= */

static void on_state_changed(const char *entity_id, const ha_ws_entity_state_t *st, void *ctx)
{
    (void)ctx;
    int idx = find_tile_index(entity_id);
    if (idx < 0) {
        return;
    }

    /* Short timeout: if the LVGL task is momentarily busy, skip this update
     * rather than block ha_ws_task - the 2 s reconciliation timer below is
     * the self-healing safety net for a burst of near-simultaneous changes
     * (e.g. a scene activation) that misses this window. */
    if (!ha_lvgl_lock(50)) {
        return;
    }
    apply_state_to_tile((size_t)idx, st);
    ha_lvgl_unlock();
}

static void on_conn_state(ha_ws_conn_state_t state, void *ctx)
{
    (void)ctx;
    if (!ha_lvgl_lock(50)) {
        return;
    }
    switch (state) {
    case HA_WS_READY:
        lv_obj_add_flag(s_status_banner, LV_OBJ_FLAG_HIDDEN);
        break;
    case HA_WS_CONNECTING:
        lv_label_set_text(s_status_label, "Connecting to Home Assistant...");
        lv_obj_remove_flag(s_status_banner, LV_OBJ_FLAG_HIDDEN);
        break;
    case HA_WS_DISCONNECTED:
    default:
        lv_label_set_text(s_status_label, "Disconnected from Home Assistant - retrying...");
        lv_obj_remove_flag(s_status_banner, LV_OBJ_FLAG_HIDDEN);
        break;
    }
    ha_lvgl_unlock();
}

/* ===========================================================================
 * Periodic timers - both run from inside lv_timer_handler() on the LVGL
 * task, which already holds ha_lvgl's lock for the duration, so neither
 * needs to take it again.
 * ========================================================================= */

static void reconcile_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (size_t i = 0; i < HA_DASHBOARD_TILE_COUNT; i++) {
        ha_ws_entity_state_t st;
        if (ha_ws_get_state(HA_DASHBOARD_TILES[i].entity_id, &st)) {
            apply_state_to_tile(i, &st);
        }
    }
}

/*
 * Three backlight stages, driven entirely off LVGL's own input-activity
 * clock: full brightness -> dimmed after HA_DISPLAY_IDLE_TIMEOUT_S -> off
 * after a further HA_DISPLAY_OFF_DELAY_S. Any touch resets
 * lv_display_get_inactive_time() to ~0 automatically (LVGL's indev
 * processing does this, not this timer), so the very next tick here sees
 * "awake" again and restores full brightness directly from off - no
 * intermediate dim step on the way back up.
 */
typedef enum { HA_BACKLIGHT_AWAKE, HA_BACKLIGHT_DIMMED, HA_BACKLIGHT_OFF } ha_backlight_state_t;

static void backlight_timer_cb(lv_timer_t *t)
{
    (void)t;
    static ha_backlight_state_t s_state = HA_BACKLIGHT_AWAKE;

    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    uint32_t dim_ms   = (uint32_t)CONFIG_HA_DISPLAY_IDLE_TIMEOUT_S * 1000;
    uint32_t off_ms   = dim_ms + (uint32_t)CONFIG_HA_DISPLAY_OFF_DELAY_S * 1000;

    ha_backlight_state_t want;
    if (idle_ms >= off_ms) {
        want = HA_BACKLIGHT_OFF;
    } else if (idle_ms >= dim_ms) {
        want = HA_BACKLIGHT_DIMMED;
    } else {
        want = HA_BACKLIGHT_AWAKE;
    }

    if (want == s_state) {
        return;
    }
    s_state = want;

    switch (want) {
    case HA_BACKLIGHT_OFF:
        ha_panel_set_backlight(0);
        break;
    case HA_BACKLIGHT_DIMMED:
        ha_panel_set_backlight(CONFIG_HA_DISPLAY_DIM_PERCENT);
        break;
    case HA_BACKLIGHT_AWAKE:
    default:
        ha_panel_set_backlight(CONFIG_HA_DISPLAY_BRIGHTNESS_PERCENT);
        break;
    }
}

/* ===========================================================================
 * Tap handling
 *
 * lv_switch is LV_OBJ_FLAG_CHECKABLE, so LVGL itself flips the switch's
 * checked state before this callback runs - that flip *is* the optimistic
 * feedback, for free. homeassistant.toggle (rather than light.toggle /
 * switch.toggle) works for both tile kinds with no branching and no need to
 * guess the current state.
 * ========================================================================= */

static void tile_toggle_cb(lv_event_t *e)
{
    const ha_dashboard_entity_t *ent = (const ha_dashboard_entity_t *)lv_event_get_user_data(e);

    esp_err_t err = ha_ws_call_service("homeassistant", "toggle", ent->entity_id, NULL);
    if (err != ESP_OK) {
        /* Not connected - undo LVGL's own optimistic flip immediately rather
         * than waiting on the reconciliation timer, since we already know
         * for certain nothing was sent. */
        lv_obj_t *sw = lv_event_get_target(e);
        if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        ESP_LOGW(TAG, "tap on %s ignored: not connected to Home Assistant", ent->entity_id);
    }
    /* If the call was sent but fails on Home Assistant's side, no
     * state_changed event follows and reconcile_timer_cb() pulls the switch
     * back to the real state within ~2 s - bounded, self-correcting, no
     * bespoke rollback path needed here. */
}

/*
 * Brightness slider - HA_TILE_LIGHT only. Unlike the switch, LVGL's own drag
 * feedback is not "confirmation enough" on its own: dragging fires
 * LV_EVENT_VALUE_CHANGED continuously, and calling call_service on every
 * intermediate tick would flood the WebSocket connection and Home Assistant
 * both. So the network call only happens on release, and slider_editing is
 * set for the whole press so apply_state_to_tile() doesn't fight the drag
 * with a stale server value arriving mid-gesture.
 */
static void slider_pressed_cb(lv_event_t *e)
{
    ha_dashboard_tile_t *tile = (ha_dashboard_tile_t *)lv_event_get_user_data(e);
    tile->slider_editing = true;
}

static void slider_released_cb(lv_event_t *e)
{
    ha_dashboard_tile_t *tile = (ha_dashboard_tile_t *)lv_event_get_user_data(e);
    tile->slider_editing = false;

    size_t                       idx = (size_t)(tile - s_tiles);
    const ha_dashboard_entity_t *ent = &HA_DASHBOARD_TILES[idx];
    int32_t                      value = lv_slider_get_value(lv_event_get_target(e));

    /* Brightness is a light-only concept - unlike tile_toggle_cb's
     * domain-agnostic homeassistant.toggle, there is no generic
     * "set_brightness" service, so this hardcodes light.turn_on. */
    ha_ws_service_data_t data = { .brightness_pct = (int)value };
    esp_err_t err = ha_ws_call_service("light", "turn_on", ent->entity_id, &data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "brightness change on %s ignored: not connected", ent->entity_id);
        /* No optimistic flip to undo here (unlike the switch) - the slider
         * already shows the value the user dragged it to, and the next
         * reconcile pass (or the next real update once reconnected) will
         * correct it if that was wrong. */
    }
}

/* ===========================================================================
 * Building the UI
 * ========================================================================= */

static lv_obj_t *make_tile(lv_obj_t *parent, size_t idx)
{
    const ha_dashboard_entity_t *ent = &HA_DASHBOARD_TILES[idx];
    ha_dashboard_tile_t          *tile = &s_tiles[idx];
    bool actuatable = (ent->kind == HA_TILE_LIGHT || ent->kind == HA_TILE_SWITCH);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161c24), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    tile->card = card;

    /* Icon: a plain power glyph rather than a per-domain bulb/switch icon -
     * LVGL's built-in symbol set has no lightbulb, and recoloring one
     * recognizable "actuatable" icon on state is more useful here than a
     * static per-domain one would be. Sensors get none; nothing to actuate,
     * nothing to indicate on/off for. */
    if (actuatable) {
        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, LV_SYMBOL_POWER);
        lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);
        tile->icon = icon;
    } else {
        tile->icon = NULL;
    }

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, ent->label);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, actuatable ? 28 : 0, 0);
    tile->title_lbl = title;

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "connecting...");
    lv_obj_set_style_text_color(value, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    tile->value_lbl = value;

    if (actuatable) {
        lv_obj_t *sw = lv_switch_create(card);
        lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_add_event_cb(sw, tile_toggle_cb, LV_EVENT_CLICKED, (void *)ent);
        tile->sw = sw;
    } else {
        tile->sw = NULL;
    }

    if (ent->kind == HA_TILE_LIGHT) {
        lv_obj_t *slider = lv_slider_create(card);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_range(slider, 1, 100);
        lv_slider_set_value(slider, 100, LV_ANIM_OFF);
        lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 0, 56);
        lv_obj_add_flag(slider, LV_OBJ_FLAG_HIDDEN); /* shown once a brightness is known */
        lv_obj_add_event_cb(slider, slider_pressed_cb, LV_EVENT_PRESSED, tile);
        lv_obj_add_event_cb(slider, slider_released_cb, LV_EVENT_RELEASED, tile);
        tile->brightness_slider = slider;
    } else {
        tile->brightness_slider = NULL;
    }

    return card;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- status banner, hidden once Home Assistant is ready --- */
    s_status_banner = lv_obj_create(scr);
    lv_obj_set_size(s_status_banner, ha_panel_width(), 48);
    lv_obj_set_pos(s_status_banner, 0, 0);
    lv_obj_set_style_bg_color(s_status_banner, lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
    lv_obj_set_style_border_width(s_status_banner, 0, 0);
    lv_obj_set_style_radius(s_status_banner, 0, 0);
    lv_obj_remove_flag(s_status_banner, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(s_status_banner);
    lv_label_set_text(s_status_label, "Starting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_center(s_status_label);

    /* --- tile grid ---
     *
     * LV_LAYOUT_GRID with fixed-size tracks rather than the previous
     * flex-wrap flow: every tile lands in an exact row/column cell instead
     * of wherever the wrap happened to put it, so tiles line up cleanly
     * regardless of how many precede them. The descriptor arrays are
     * function-static (not stack-local) because LVGL keeps the pointer for
     * as long as the grid object lives, which here is forever - the
     * dashboard is built once and never torn down.
     */
    static int32_t s_col_dsc[HA_GRID_COLS + 1];
    static int32_t s_row_dsc[HA_GRID_ROWS + 1];
    for (int c = 0; c < HA_GRID_COLS; c++) {
        s_col_dsc[c] = HA_TILE_W;
    }
    s_col_dsc[HA_GRID_COLS] = LV_GRID_TEMPLATE_LAST;
    for (int r = 0; r < HA_GRID_ROWS; r++) {
        s_row_dsc[r] = HA_TILE_H;
    }
    s_row_dsc[HA_GRID_ROWS] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, ha_panel_width(), ha_panel_height() - 48);
    lv_obj_set_pos(grid, 0, 48);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, HA_TILE_GAP, 0);
    lv_obj_set_style_pad_row(grid, HA_TILE_GAP, 0);
    lv_obj_set_style_pad_column(grid, HA_TILE_GAP, 0);
    lv_obj_set_grid_dsc_array(grid, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < HA_DASHBOARD_TILE_COUNT; i++) {
        lv_obj_t *card = make_tile(grid, i);
        int col = (int)(i % HA_GRID_COLS);
        int row = (int)(i / HA_GRID_COLS);
        lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    }

    lv_timer_create(reconcile_timer_cb, 2000, NULL);
    lv_timer_create(backlight_timer_cb, 1000, NULL);

    ha_panel_set_backlight(CONFIG_HA_DISPLAY_BRIGHTNESS_PERCENT);
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t ha_dashboard_init(void)
{
    if (!ha_lvgl_lock(0)) {
        return ESP_FAIL;
    }
    build_ui();
    ha_lvgl_unlock();

    ha_ws_set_state_cb(on_state_changed, NULL);
    ha_ws_set_conn_cb(on_conn_state, NULL);

    /* In case ha_ws.c already has cached data by the time this runs (it
     * won't on a fresh boot - ha_ws_start() is called after this - but will
     * on any code path that rebuilds the dashboard after ha_ws has already
     * connected once). */
    if (ha_lvgl_lock(0)) {
        for (size_t i = 0; i < HA_DASHBOARD_TILE_COUNT; i++) {
            ha_ws_entity_state_t st;
            if (ha_ws_get_state(HA_DASHBOARD_TILES[i].entity_id, &st)) {
                apply_state_to_tile(i, &st);
            }
        }
        ha_lvgl_unlock();
    }

    ESP_LOGI(TAG, "dashboard ready (%u tiles)", (unsigned)HA_DASHBOARD_TILE_COUNT);
    return ESP_OK;
}

void ha_dashboard_wake_backlight(void)
{
    if (!ha_lvgl_lock(50)) {
        return;
    }
    lv_display_trigger_activity(NULL);
    ha_lvgl_unlock();
    ha_panel_set_backlight(CONFIG_HA_DISPLAY_BRIGHTNESS_PERCENT);
}
