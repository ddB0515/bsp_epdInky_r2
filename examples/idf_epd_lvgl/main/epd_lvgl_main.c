/*
 * epdInky ESP32-P4 - LVGL on a raw E Ink panel
 *
 * Shows how to drive an LVGL interface on a panel whose refresh costs about
 * 1.4 s and which has no usable partial update. The bridge in app_epd_lvgl.c
 * does the interesting part; this file brings up the board and builds a UI.
 *
 * The rule this example is built around: let LVGL run freely into memory, and
 * treat reaching the glass as a separate, expensive, deliberate act.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "epd_display.h"
#include "epd_panel.h"
#include "epd_panels.h"

#include "app_epd_lvgl.h"

static const char *TAG = "epd_lvgl_demo";

/*
 * How the panel connector is wired to this board. Every entry comes from the
 * BSP pin map; see the raw EPD example for the pins that look free and are
 * not - GPIO 24/25 are USB, GPIO 0/1 are the 32 kHz crystal.
 */
static const epd_board_config_t s_board = {
	.data           = BSP_EPD_DATA_PINS_DEFAULT,
	.cl             = BSP_EPD_PIN_XCL,
	.le             = BSP_EPD_PIN_XLE,
	.oe             = BSP_EPD_PIN_XOE,
	.sph            = BSP_EPD_PIN_XSTL,
	.spv            = BSP_EPD_PIN_SPV,
	.ckv            = BSP_EPD_PIN_CKV,
	.gmod           = BSP_EPD_PIN_MODE,
	.dc_dummy       = GPIO_NUM_36,
	.oe_active_high = true,
};

/* Greyscale, not colour: LVGL renders L8 and the panel shows 16 levels. */
#define GREY(v) lv_color_make((v), (v), (v))

static lv_obj_t *s_counter_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_btn_plus;
static lv_obj_t *s_btn_minus;
static lv_obj_t *s_switch;
static lv_obj_t *s_slider;
static int       s_counter;

static void set_status(const char *text)
{
	if (s_status_label) {
		lv_label_set_text(s_status_label, text);
	}
}

static void count_cb(lv_event_t *e)
{
	int delta = (int)(intptr_t)lv_event_get_user_data(e);
	s_counter += delta;
	lv_label_set_text_fmt(s_counter_label, "%d", s_counter);
	set_status(delta > 0 ? "counter + 1" : "counter - 1");
}

static void refresh_cb(lv_event_t *e)
{
	(void)e;
	set_status("manual refresh requested");
	app_epd_lvgl_request_refresh();
}

static void clean_cb(lv_event_t *e)
{
	(void)e;
	set_status("deep clean requested");
	app_epd_lvgl_request_clean();
}

/*
 * Emulated input.
 *
 * Set to 0 on a board that has a touchscreen: then the widgets are driven by
 * real presses and this whole section compiles out.
 *
 * With it set to 1 the demo taps its own buttons. That is worth doing even
 * with touch available, because it makes the example self-verifying - a static
 * screen proves nothing, whereas a counter that visibly counts proves the
 * whole chain from an LVGL event through the shadow buffer to the glass.
 */
#define DEMO_EMULATE_TAPS 1

#if DEMO_EMULATE_TAPS

/*
 * Milliseconds between emulated steps.
 *
 * Must comfortably exceed settle_ms plus a refresh. A refresh here is about
 * 3.1 s because the rails are cycled around it (see keep_rails_on), so this
 * needs to stay well above that - if a step lands while the previous refresh
 * is still running, the settle policy correctly folds the two together and the
 * intermediate value never reaches the panel.
 */
#define DEMO_STEP_MS 5000

typedef enum {
	TAP_PLUS,        /* click the "+ 1" button        */
	TAP_MINUS,       /* click the "- 1" button        */
	TOGGLE_SWITCH,   /* flip the switch               */
	SET_SLIDER,      /* move the slider to step.arg   */
} demo_action_t;

typedef struct {
	demo_action_t action;
	int32_t       arg;
} demo_step_t;

/* Up a few times, then down once, so a change in direction is visible too. */
static const demo_step_t s_script[] = {
	{ TAP_PLUS,      0  },
	{ TAP_PLUS,      0  },
	{ TAP_PLUS,      0  },
	{ TAP_MINUS,     0  },
	{ TOGGLE_SWITCH, 0  },   /* off */
	{ TOGGLE_SWITCH, 0  },   /* and back on */
	{ SET_SLIDER,    85 },
	{ SET_SLIDER,    15 },
};

static size_t s_step;

/*
 * Emulate a tap.
 *
 * Sends the event the button would send rather than calling the handler
 * directly, so the widget's own event dispatch is exercised and the emulated
 * path stays identical to the touch path.
 */
static void tap(lv_obj_t *obj)
{
	if (obj) {
		lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
	}
}

static void demo_script_cb(lv_timer_t *t)
{
	if (s_step >= sizeof(s_script) / sizeof(s_script[0])) {
		set_status("emulated sequence complete");
		lv_timer_delete(t);
		return;
	}

	const demo_step_t *step = &s_script[s_step++];

	switch (step->action) {
	case TAP_PLUS:
		tap(s_btn_plus);
		break;

	case TAP_MINUS:
		tap(s_btn_minus);
		break;

	case TOGGLE_SWITCH:
		if (s_switch) {
			bool on = lv_obj_has_state(s_switch, LV_STATE_CHECKED);
			if (on) {
				lv_obj_remove_state(s_switch, LV_STATE_CHECKED);
			} else {
				lv_obj_add_state(s_switch, LV_STATE_CHECKED);
			}
			/* Changing state does not notify by itself. */
			lv_obj_send_event(s_switch, LV_EVENT_VALUE_CHANGED, NULL);
			set_status(on ? "switch off" : "switch on");
		}
		break;

	case SET_SLIDER:
		if (s_slider) {
			lv_slider_set_value(s_slider, step->arg, LV_ANIM_OFF);
			lv_obj_send_event(s_slider, LV_EVENT_VALUE_CHANGED, NULL);
			set_status("slider moved");
		}
		break;
	}
}

static void demo_emulation_start(void)
{
	if (s_btn_plus || s_switch) {
		lv_timer_create(demo_script_cb, DEMO_STEP_MS, NULL);
	}
}

#else /* DEMO_EMULATE_TAPS */

static void demo_emulation_start(void) { }

#endif /* DEMO_EMULATE_TAPS */

/*
 * A button sized for a 10 inch e-paper panel.
 *
 * Deliberately larger and higher contrast than a backlit UI would need: this
 * panel is 1872x1404 over 10.3 inches, so LVGL's defaults come out physically
 * small, and E Ink's contrast is lower than an emissive display.
 */
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y,
                             lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_set_size(btn, 300, 110);
	lv_obj_set_pos(btn, x, y);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

	lv_obj_set_style_bg_color(btn, GREY(0x00), LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(btn, GREY(0x80), LV_STATE_PRESSED);
	lv_obj_set_style_radius(btn, 12, 0);

	lv_obj_t *lab = lv_label_create(btn);
	lv_label_set_text(lab, text);
	lv_obj_set_style_text_color(lab, GREY(0xFF), 0);
	lv_obj_center(lab);
	return btn;
}

/* Set to 1 to reduce the UI to a single label, to tell a bridge problem from
 * a widget problem. */
#define MINIMAL_UI 0

static void build_ui(void)
{
#if MINIMAL_UI
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, GREY(0xFF), 0);
	lv_obj_t *l = lv_label_create(scr);
	lv_label_set_text(l, "LVGL on E Ink - minimal");
	lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(l, GREY(0x00), 0);
	lv_obj_set_pos(l, 100, 100);
	s_counter_label = lv_label_create(scr);
	lv_label_set_text(s_counter_label, "0");
	lv_obj_set_pos(s_counter_label, 100, 220);
	s_status_label = lv_label_create(scr);
	lv_label_set_text(s_status_label, "ready");
	lv_obj_set_pos(s_status_label, 100, 300);
	return;
#else
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, GREY(0xFF), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	/*
	 * No scrolling, and no scrollbars.
	 *
	 * Adding children makes an LVGL object scrollable by default, and the
	 * scrollbar's auto-hide is animated. On a normal display that is a fading
	 * bar nobody notices; here it invalidates the screen continuously, and
	 * with a full-screen UI it kept lv_timer_handler() busy indefinitely -
	 * the LVGL task never yielded and the idle-task watchdog fired. Nothing
	 * scrolls on a fixed e-paper layout, so turn it off.
	 */
	lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* Title */
	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "LVGL on E Ink");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(title, GREY(0x00), 0);
	lv_obj_set_pos(title, 60, 50);

	lv_obj_t *sub = lv_label_create(scr);
	lv_label_set_text(sub, "ED103TC2  1872x1404  16 grey levels  L8 rendering");
	lv_obj_set_style_text_color(sub, GREY(0x40), 0);
	lv_obj_set_pos(sub, 60, 120);

	lv_obj_t *rule = lv_obj_create(scr);
	lv_obj_set_size(rule, 1752, 4);
	lv_obj_set_pos(rule, 60, 175);
	lv_obj_set_style_bg_color(rule, GREY(0x00), 0);
	lv_obj_set_style_border_width(rule, 0, 0);
	lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);

	/* Grey ramp: LVGL widgets rendering the panel's actual tonal range. */
	lv_obj_t *ramp_lab = lv_label_create(scr);
	lv_label_set_text(ramp_lab, "16 grey levels, drawn as LVGL objects");
	lv_obj_set_style_text_color(ramp_lab, GREY(0x00), 0);
	lv_obj_set_pos(ramp_lab, 60, 210);

	for (int i = 0; i < 16; i++) {
		lv_obj_t *sw = lv_obj_create(scr);
		lv_obj_set_size(sw, 108, 130);
		lv_obj_set_pos(sw, 60 + i * 109, 260);
		lv_obj_set_style_bg_color(sw, GREY(i * 17), 0);   /* 0..255 in 16 steps */
		lv_obj_set_style_border_width(sw, 0, 0);
		lv_obj_set_style_radius(sw, 0, 0);
		lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
	}

	/* Counter, to show the refresh policy coalescing rapid changes. */
	lv_obj_t *cnt_lab = lv_label_create(scr);
	lv_label_set_text(cnt_lab, "Counter");
	lv_obj_set_style_text_color(cnt_lab, GREY(0x00), 0);
	lv_obj_set_pos(cnt_lab, 60, 450);

	s_counter_label = lv_label_create(scr);
	lv_label_set_text(s_counter_label, "0");
	lv_obj_set_style_text_font(s_counter_label, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(s_counter_label, GREY(0x00), 0);
	lv_obj_set_pos(s_counter_label, 60, 500);

	s_btn_minus = make_button(scr, "- 1", 300, 480, count_cb, (void *)(intptr_t)-1);
	s_btn_plus  = make_button(scr, "+ 1", 640, 480, count_cb, (void *)(intptr_t)1);
	make_button(scr, "Refresh", 980, 480, refresh_cb, NULL);
	make_button(scr, "Deep clean", 1320, 480, clean_cb, NULL);
	/* A few widgets, to show the theme rendering in greyscale. */
	lv_obj_t *bar = lv_bar_create(scr);
	lv_obj_set_size(bar, 700, 50);
	lv_obj_set_pos(bar, 60, 680);
	lv_bar_set_value(bar, 65, LV_ANIM_OFF);

	lv_obj_t *slider = lv_slider_create(scr);
	lv_obj_set_size(slider, 700, 50);
	lv_obj_set_pos(slider, 60, 770);
	lv_slider_set_value(slider, 40, LV_ANIM_OFF);
	s_slider = slider;

	lv_obj_t *sw = lv_switch_create(scr);
	lv_obj_set_size(sw, 120, 60);
	lv_obj_set_pos(sw, 860, 680);
	lv_obj_add_state(sw, LV_STATE_CHECKED);
	s_switch = sw;

	lv_obj_t *cb = lv_checkbox_create(scr);
	lv_checkbox_set_text(cb, "Checkbox");
	lv_obj_set_style_text_color(cb, GREY(0x00), 0);
	lv_obj_set_pos(cb, 860, 780);

	lv_obj_t *panel = lv_obj_create(scr);
	lv_obj_set_size(panel, 700, 240);
	lv_obj_set_pos(panel, 1100, 660);
	lv_obj_set_style_bg_color(panel, GREY(0xFF), 0);
	lv_obj_set_style_border_color(panel, GREY(0x00), 0);
	lv_obj_set_style_border_width(panel, 3, 0);
	lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
	lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *ptext = lv_label_create(panel);
	lv_label_set_text(ptext,
	                  "Containers, borders and text\n"
	                  "all render in greyscale.\n\n"
	                  "No colour conversion: LVGL\n"
	                  "draws L8 and the panel takes\n"
	                  "the top nibble.");
	lv_obj_set_style_text_color(ptext, GREY(0x00), 0);

	/* Status line, updated by the button handlers. */
	s_status_label = lv_label_create(scr);
	lv_obj_set_style_text_color(s_status_label, GREY(0x40), 0);
	lv_obj_set_pos(s_status_label, 60, 950);
	set_status("ready");

	lv_obj_t *note = lv_label_create(scr);
	lv_label_set_text(note,
	                  "LVGL renders continuously into memory. The panel is only\n"
	                  "refreshed once the screen stops changing, which is what\n"
	                  "keeps a 1.4 s refresh from being paid several times per\n"
	                  "interaction.");
	lv_obj_set_style_text_color(note, GREY(0x40), 0);
	lv_obj_set_pos(note, 60, 1010);
#endif
}

void app_main(void)
{
	ESP_LOGI(TAG, "epdInky ESP32-P4 LVGL on E Ink");

	/*
	 * I2C and the PMIC only. The epd component owns the panel pins, so the
	 * BSP's EPD GPIO helper is left off to avoid two owners.
	 */
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tps65185 = true;
	cfg.enable.use_epd_gpio = false;
	cfg.enable.use_tca6408  = false;
	cfg.enable.use_kxtj3    = false;
	cfg.enable.use_rv3028   = false;
	cfg.enable.use_stc3115  = false;
	cfg.enable.use_sdcard   = false;

	bsp_epdinky_handles_t board;
	ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));

	epd_panel_def_t panel_def = epd_panel_eink_ed103tc2;

	epd_panel_handle_t panel = NULL;
	ESP_ERROR_CHECK(epd_display_panel_create(&panel_def, &s_board,
	                                         board.tps65185, &panel));
	ESP_LOGI(TAG, "Panel ready: %s %ux%u, VCOM -%u mV",
	         panel_def.name, panel_def.width, panel_def.height,
	         panel_def.vcom_mv);

	/*
	 * Deliberately NOT powered on here.
	 *
	 * The bridge brings the rails up for each update and drops them again
	 * afterwards. Holding them up across an idle UI leaves every pixel under a
	 * DC bias of the full VCOM magnitude, which makes white go grainy over a
	 * few minutes. See app_epd_lvgl_config_t::keep_rails_on.
	 */

	const app_epd_lvgl_config_t lv_cfg = {
		.panel        = panel,
		.settle_ms    = 400,     /* coalesce a burst of changes into one refresh */
		.max_defer_ms = 5000,    /* but never let a change wait longer than this */
		.mode         = APP_EPD_LVGL_FULL,
		.startup_clean_cycles = 3,
	};
	ESP_ERROR_CHECK(app_epd_lvgl_init(&lv_cfg));

	if (app_epd_lvgl_lock(0)) {
		build_ui();
		demo_emulation_start();
		app_epd_lvgl_unlock();
	}

	/*
	 * The LVGL task owns everything from here: timers, the startup clean, the
	 * first screen and every refresh after it.
	 */
	ESP_LOGI(TAG, "Starting LVGL");
	ESP_ERROR_CHECK(app_epd_lvgl_start());
}
