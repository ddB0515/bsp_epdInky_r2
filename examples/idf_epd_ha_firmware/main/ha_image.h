/*
 * Inbound image decoding.
 *
 * This firmware fetches whatever PNG a user-configured dashboard-render
 * service returns (see ha_http.h) and puts it on the panel - it does not
 * render anything itself. The decoder below is protocol-agnostic: it
 * implements baseline PNG - colour types 0/2/3/4/6, bit depths 1/2/4/8/16 and
 * all five row filters - plus a small 1-bpp BMP path for convenience, since
 * some render services default to BMP for monochrome output.
 *
 * Interlaced (Adam7) PNGs are rejected; supporting them would roughly double
 * the code for a case a server-side render pipeline has no reason to produce.
 *
 * Inflate comes from the ESP32-P4 boot ROM (tinfl, see
 * components/esp_rom/esp32p4/ld/esp32p4.rom.ld), so this costs no flash and
 * pulls in neither libpng nor zlib.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "epd_fb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What an image's header says about it, without decoding the pixels. */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  bit_depth;
    uint8_t  colour_type;   /**< 0 grey, 2 RGB, 3 indexed, 4 grey+A, 6 RGBA */
    bool     interlaced;
} ha_png_info_t;

/**
 * @brief  Read a PNG's IHDR without decoding it.
 *
 * Useful for logging and for rejecting an image before allocating anything.
 *
 * @return ESP_ERR_INVALID_ARG    not a PNG, or the header is truncated
 *         ESP_ERR_INVALID_SIZE   dimensions outside what this decoder accepts
 */
esp_err_t ha_png_probe(const uint8_t *data, size_t len, ha_png_info_t *out);

/**
 * @brief  Decode a PNG into @p fb, converting to 4-bit grey.
 *
 * When the image is exactly the framebuffer's size it is written straight in,
 * row by row - which is the case when the render service is pointed at the
 * panel's own resolution, as the README asks. Otherwise it is decoded to a
 * scratch buffer and then rotated, scaled and centred with
 * epd_fb_blit_fit_rot(EPD_ROT_AUTO), letterboxed in white, so a mismatched
 * render still produces something sensible rather than an error.
 *
 * Colour is reduced by luminance (Rec. 601), alpha is composited over white.
 *
 * Peak extra memory is the inflated image, which for a 1872x1404 4bpp frame is
 * about 1.29 MB, taken from PSRAM.
 *
 * @return ESP_ERR_INVALID_ARG    not a PNG, or malformed
 *         ESP_ERR_NOT_SUPPORTED  interlaced, or a colour/depth combination PNG
 *                                does not define
 *         ESP_ERR_NO_MEM         could not allocate the inflate buffer
 */
esp_err_t ha_png_render(const uint8_t *data, size_t len, epd_fb_t *fb);

/**
 * @brief  Decode an uncompressed 1-bpp BMP into @p fb, scaled to fit.
 *
 * Only BITMAPINFOHEADER with a 2-entry palette is supported, which is what
 * every monochrome BMP writer produces; which palette entry is black is read
 * from the file rather than assumed, and rows may be bottom-up or top-down
 * (BMP's own sign-of-height convention) - both handled via epd_bitmap1_t
 * rather than a copy/flip pass.
 *
 * @return ESP_ERR_INVALID_ARG    not a BMP, or malformed/truncated
 *         ESP_ERR_NOT_SUPPORTED  compressed, multi-plane, or not 1 bpp
 *         ESP_ERR_INVALID_SIZE   dimensions outside what this decoder accepts
 */
esp_err_t ha_bmp_render(const uint8_t *data, size_t len, epd_fb_t *fb);

/**
 * @brief  Decode whatever @p data holds into @p fb, dispatching on its magic.
 *
 * PNG and 1-bpp BMP are both decoded. JPEG and anything else is logged by name
 * (where recognisable) and rejected with ESP_ERR_NOT_SUPPORTED rather than
 * silently failing - point the render service at PNG instead.
 */
esp_err_t ha_image_render(const uint8_t *data, size_t len, epd_fb_t *fb);

#ifdef __cplusplus
}
#endif
