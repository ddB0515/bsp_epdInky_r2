#include "ha_display.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "bsp/epdinky_p4_board.h"
#include "epd_display.h"
#include "epd_panels.h"

#include "ha_image.h"
#include "qrcodegen.h"

static const char *TAG = "ha_display";

/*
 * [HW] Pin map for the parallel source bus, copied from examples/idf_epd_raw
 * and examples/trmnl-firmware (same board, same panel connector).
 *
 * dc_dummy is the one entry that is not a real signal. The i80 peripheral
 * insists on a data/command GPIO whether the panel has one or not, so it is
 * pointed at GPIO36: a pin with a 10K pull-up (R50) that this board does not
 * otherwise use. GPIO24/25 are USB_DN/USB_DP and GPIO38 is the TPS65185
 * interrupt - none of those can stand in for it.
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

/* Full black/white cycles used when ha_display_flush() is asked to clean. */
#define HA_CLEAN_CYCLES 1

static epd_panel_handle_t s_panel;
static epd_fb_t           s_fb;
static bool               s_ready;

/* -------------------------------------------------------------------------- */

esp_err_t ha_display_init(tps65185_handle_t pmic)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (pmic == NULL) {
        ESP_LOGE(TAG, "no PMIC handle; the panel has no rails");
        return ESP_ERR_INVALID_ARG;
    }

    /* The catalogue entry for whichever panel Kconfig says is physically
     * connected - see the HA_PANEL choice in Kconfig.projbuild. Unlike the
     * trmnl-firmware example, HA_DISPLAY_WIDTH/HEIGHT (ha_config.h) follow
     * this choice directly, so a server-side render at the wrong size is a
     * user misconfiguration to fix (point the render service at the right
     * resolution), not something this firmware needs to silently absorb -
     * though ha_image.c still falls back to a scaled fit if it happens. */
#if CONFIG_HA_PANEL_ED133UT2
    epd_panel_def_t def = epd_panel_eink_ed133ut2;
#else
    epd_panel_def_t def = epd_panel_eink_ed103tc2;
#endif

    esp_err_t err = epd_display_panel_create(&def, &s_board, pmic, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel create failed: %s", esp_err_to_name(err));
        return err;
    }

    err = epd_fb_create(&s_fb, def.width, def.height);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "framebuffer alloc failed: %s", esp_err_to_name(err));
        epd_panel_destroy(s_panel);
        s_panel = NULL;
        return err;
    }
    epd_fb_fill(&s_fb, 0x0F);

    s_ready = true;
    ESP_LOGI(TAG, "panel ready: %s %ux%u", def.name, s_fb.width, s_fb.height);
    return ESP_OK;
}

epd_fb_t *ha_display_fb(void)
{
    return s_ready ? &s_fb : NULL;
}

esp_err_t ha_display_flush(bool clean)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = epd_panel_power_on(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power on failed: %s", esp_err_to_name(err));
        return err;
    }

    if (clean) {
        err = epd_panel_clean(s_panel, HA_CLEAN_CYCLES);
    }
    if (err == ESP_OK) {
        /* INIT lays down the white baseline GC16 needs; see the header. */
        err = epd_panel_refresh(s_panel, NULL, s_fb.buf, EPD_WAVEFORM_INIT);
    }
    if (err == ESP_OK) {
        err = epd_panel_refresh(s_panel, NULL, s_fb.buf, EPD_WAVEFORM_GC16);
    }

    /* Always drop the rails, even on a failed refresh - leaving VCOM live is
     * what makes white areas go grainy. */
    const esp_err_t off = epd_panel_power_off(s_panel);
    if (err == ESP_OK) {
        err = off;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ha_display_show_image(const uint8_t *data, size_t len, bool clean)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Decode first, draw second. If the image is malformed the framebuffer
     * still holds whatever was there before, so a bad frame from the render
     * service costs the user nothing - the panel keeps showing the last good
     * one.
     */
    esp_err_t err = ha_image_render(data, len, &s_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "decode failed: %s - leaving the current frame up", esp_err_to_name(err));
        return err;
    }

    return ha_display_flush(clean);
}

/* -------------------------------------------------------------------------- */
/* Status screens                                                             */
/* -------------------------------------------------------------------------- */

#define HEADING_SCALE 8     /* 64 px tall */
#define BODY_SCALE    4     /* 32 px */
#define FOOTER_SCALE  3     /* 24 px */

#define GLYPH_PX      8

/** Draw @p text centred on @p y (its top edge), returning the height consumed. */
static uint16_t draw_centred(epd_fb_t *fb, uint16_t y, const char *text, uint8_t scale)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }

    const size_t   len = strlen(text);
    const uint32_t w   = (uint32_t)len * GLYPH_PX * scale;
    const uint16_t x   = (w < fb->width) ? (uint16_t)((fb->width - w) / 2u) : 0u;

    epd_fb_draw_string(fb, x, y, text, scale, 0x00, 0xFF);
    return (uint16_t)(GLYPH_PX * scale);
}

esp_err_t ha_display_message(const char *heading, const char *body, const char *footer)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t gap = 32;

    uint16_t total = 0;
    if (heading != NULL && *heading != '\0') { total += GLYPH_PX * HEADING_SCALE + gap; }
    if (body    != NULL && *body    != '\0') { total += GLYPH_PX * BODY_SCALE    + gap; }
    if (footer  != NULL && *footer  != '\0') { total += GLYPH_PX * FOOTER_SCALE; }

    epd_fb_fill(&s_fb, 0x0F);

    uint16_t y = (total < s_fb.height) ? (uint16_t)((s_fb.height - total) / 2u) : 0u;

    uint16_t h = draw_centred(&s_fb, y, heading, HEADING_SCALE);
    if (h != 0) { y += h + gap; }
    h = draw_centred(&s_fb, y, body, BODY_SCALE);
    if (h != 0) { y += h + gap; }
    draw_centred(&s_fb, y, footer, FOOTER_SCALE);

    ESP_LOGI(TAG, "status screen: %s | %s | %s",
             heading ? heading : "", body ? body : "", footer ? footer : "");

    /* Status screens follow whatever was on the panel, often a dense image, so
     * clean first: ghosting behind large text is very visible. */
    return ha_display_flush(true);
}

/*
 * The provisioning screen: heading and firmware version, a left-aligned text
 * column, and a WIFI:T:nopass;S:<ssid>;; QR code on the right - the payload
 * every phone's camera app already recognises as "join this network", so
 * scanning it is a shortcut around typing the SSID (see trmnl-firmware's
 * trmnl_display_show_wifi_setup(), which this mirrors). The QR only carries
 * the Wi-Fi join, not the portal URL - MQTT/dashboard configuration still
 * needs the browser step, so the text column keeps that line.
 */

/* A fixed target rather than "fill the available height": this is a landscape
 * panel with a lot of vertical room, and maximising the code would produce
 * modules far larger than scanning needs while crowding the text column that
 * sits to its left. 480 px is comfortably scannable at arm's length. */
#define WIFI_QR_TARGET_PX  480
#define WIFI_QR_QUIET      4    /* modules of white border - part of the QR spec, not decoration */

#define WIFI_MARGIN        80
#define WIFI_GAP           24
#define WIFI_HEADING_SCALE 8
#define WIFI_SUB_SCALE     3
#define WIFI_LINE_SCALE    3

esp_err_t ha_display_show_wifi_setup(const char *ap_ssid, const char *portal_url,
                                     const char *fw_version)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ap_ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    epd_fb_fill(&s_fb, 0x0F);

    /* ---- QR code, right-aligned ------------------------------------------ */

    char payload[64];
    snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ap_ssid);

    uint8_t     qr_temp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t     qr_data[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    const bool  qr_ok = qrcodegen_encodeText(payload, qr_temp, qr_data,
                                             qrcodegen_Ecc_LOW, 1, 10,
                                             qrcodegen_Mask_AUTO, true);

    uint16_t qr_px = 0;
    if (qr_ok) {
        const int      modules       = qrcodegen_getSize(qr_data);
        const int      total_modules = modules + 2 * WIFI_QR_QUIET;
        uint16_t       module_px     = (uint16_t)(WIFI_QR_TARGET_PX / total_modules);
        if (module_px < 4) {
            module_px = 4; /* stay scannable even if a longer SSID pushed the version up */
        }
        qr_px = (uint16_t)(module_px * total_modules);

        const uint16_t qr_x = s_fb.width  - WIFI_MARGIN - qr_px;
        const uint16_t qr_y = (s_fb.height - qr_px) / 2u;

        epd_fb_fill_rect(&s_fb, qr_x, qr_y, qr_px, qr_px, 0x0F);
        for (int my = 0; my < modules; my++) {
            for (int mx = 0; mx < modules; mx++) {
                if (qrcodegen_getModule(qr_data, mx, my)) {
                    epd_fb_fill_rect(&s_fb,
                                     (uint16_t)(qr_x + (WIFI_QR_QUIET + mx) * module_px),
                                     (uint16_t)(qr_y + (WIFI_QR_QUIET + my) * module_px),
                                     module_px, module_px, 0x00);
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "SSID too long to fit a QR payload; showing text only");
    }

    /* ---- Text column, left-aligned --------------------------------------- */

    char fw_line[40];
    snprintf(fw_line, sizeof(fw_line), "FW: %s", fw_version ? fw_version : "?");
    char ssid_line[48];
    snprintf(ssid_line, sizeof(ssid_line), "or connect to \"%s\" manually,", ap_ssid);
    char url_line[48];
    snprintf(url_line, sizeof(url_line), "then browse to: %s",
             portal_url ? portal_url : "http://4.3.2.1/");

    const char *lines[] = {
        qr_ok ? "Scan the QR code to join Wi-Fi directly," : "Connect a phone or computer",
        ssid_line,
        url_line,
        "to configure Wi-Fi, MQTT and the dashboard URL.",
    };

    uint16_t total = GLYPH_PX * WIFI_HEADING_SCALE + WIFI_GAP
                   + GLYPH_PX * WIFI_SUB_SCALE     + WIFI_GAP * 2;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        total += GLYPH_PX * WIFI_LINE_SCALE + (i ? 8u : 0u);
    }

    uint16_t y = (total < s_fb.height) ? (uint16_t)((s_fb.height - total) / 2u) : WIFI_MARGIN;

    epd_fb_draw_string(&s_fb, WIFI_MARGIN, y, "Set up this device", WIFI_HEADING_SCALE, 0x00, 0xFF);
    y += GLYPH_PX * WIFI_HEADING_SCALE + WIFI_GAP;

    epd_fb_draw_string(&s_fb, WIFI_MARGIN, y, fw_line, WIFI_SUB_SCALE, 0x00, 0xFF);
    y += GLYPH_PX * WIFI_SUB_SCALE + WIFI_GAP * 2;

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        epd_fb_draw_string(&s_fb, WIFI_MARGIN, y, lines[i], WIFI_LINE_SCALE, 0x00, 0xFF);
        y += GLYPH_PX * WIFI_LINE_SCALE + 8;
    }

    ESP_LOGI(TAG, "wifi setup screen: SSID \"%s\", portal %s, qr %s", ap_ssid,
             portal_url ? portal_url : "?", qr_ok ? "yes" : "no");

    return ha_display_flush(true);
}
