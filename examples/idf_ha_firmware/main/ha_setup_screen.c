#include "ha_setup_screen.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "ha_lvgl.h"
#include "ha_panel.h"

static void clear_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

void ha_setup_screen_show_wifi(const char *ap_ssid, const char *portal_url)
{
    if (!ha_lvgl_lock(0)) {
        return;
    }
    clear_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Set up this display");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(ssid_lbl, "Join Wi-Fi network \"%s\" (open)", ap_ssid);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_white(), 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *url_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(url_lbl, "Then browse to %s", portal_url);
    lv_obj_set_style_text_color(url_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(url_lbl, LV_ALIGN_TOP_MID, 0, 140);

    /* WIFI: URI format, understood by every phone's camera app for a
     * direct join - no typing the SSID by hand needed. */
    char payload[96];
    snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ap_ssid);

    lv_obj_t *qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, 320);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, payload, strlen(payload));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 60);
    /* lv_qrcode has no built-in quiet zone; a light border stands in for
     * one so phone cameras that expect margin around the code can still
     * lock onto it. */
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 16, 0);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Scan to join directly, or connect manually above");
    lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    ha_lvgl_unlock();
    ha_panel_set_backlight(CONFIG_HA_DISPLAY_BRIGHTNESS_PERCENT);
}

void ha_setup_screen_show_message(const char *title, const char *body)
{
    if (!ha_lvgl_lock(0)) {
        return;
    }
    clear_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title_lbl = lv_label_create(scr);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *body_lbl = lv_label_create(scr);
    lv_label_set_text(body_lbl, body);
    lv_obj_set_style_text_color(body_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(body_lbl, LV_ALIGN_CENTER, 0, 30);

    ha_lvgl_unlock();
    ha_panel_set_backlight(CONFIG_HA_DISPLAY_BRIGHTNESS_PERCENT);
}

void ha_setup_screen_show_config(const char *portal_url)
{
    if (!ha_lvgl_lock(0)) {
        return;
    }
    clear_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Wi-Fi connected");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Open this address to finish Home Assistant setup:");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *url_lbl = lv_label_create(scr);
    lv_label_set_text(url_lbl, portal_url);
    lv_obj_set_style_text_font(url_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(url_lbl, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(url_lbl, LV_ALIGN_CENTER, 0, 40);

    ha_lvgl_unlock();
    ha_panel_set_backlight(CONFIG_HA_DISPLAY_BRIGHTNESS_PERCENT);
}
