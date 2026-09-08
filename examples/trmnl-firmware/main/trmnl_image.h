/*
 * Inbound image decoding for the TRMNL port.
 *
 * The server sends PNG. That was established by the phase 0 spike against a
 * real account, and the two variants observed were:
 *
 *   system screens  1872x1404, bit depth 4, colour type 3 (indexed), PLTE is a
 *                   linear grey ramp running white -> black
 *   plugin renders  1872x1404, bit depth 4, colour type 0 (greyscale)
 *
 * Both are non-interlaced with every row using filter type 0, both are at the
 * panel's native resolution, and both pack two 4-bit samples per byte with the
 * leftmost pixel in the high nibble - which is exactly epd_fb's layout. Colour
 * type 0 additionally defines sample 0 as black and 15 as white, matching
 * epd_fb's convention outright, so that path is a row memcpy.
 *
 * The decoder below does not assume any of that. It implements baseline PNG:
 * colour types 0/2/3/4/6, bit depths 1/2/4/8/16 and all five row filters, with
 * the observed shapes taken as fast paths. Interlaced (Adam7) images are
 * rejected - the server does not produce them and supporting them would roughly
 * double the code.
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
} trmnl_png_info_t;

/**
 * @brief  Read a PNG's IHDR without decoding it.
 *
 * Useful for logging and for rejecting an image before allocating anything.
 *
 * @return ESP_ERR_INVALID_ARG    not a PNG, or the header is truncated
 *         ESP_ERR_INVALID_SIZE   dimensions outside what this decoder accepts
 */
esp_err_t trmnl_png_probe(const uint8_t *data, size_t len, trmnl_png_info_t *out);

/**
 * @brief  Decode a PNG into @p fb, converting to 4-bit grey.
 *
 * When the image is exactly the framebuffer's size it is written straight in,
 * row by row. Otherwise it is decoded to a scratch buffer and then rotated,
 * scaled and centred with epd_fb_blit_fit_rot(EPD_ROT_AUTO), letterboxed in
 * white - so a server that starts sending a different geometry still produces
 * something sensible rather than an error.
 *
 * Colour is reduced by luminance (Rec. 601), alpha is composited over white.
 *
 * Peak extra memory is the inflated image, which for a 1872x1404 4bpp frame is
 * about 1.29 MB, taken from PSRAM. That is deliberate: the alternative is a
 * 32 KB sliding window with row reassembly, which is more code for a saving
 * this board (32 MB PSRAM) has no use for.
 *
 * @return ESP_ERR_INVALID_ARG    not a PNG, or malformed
 *         ESP_ERR_NOT_SUPPORTED  interlaced, or a colour/depth combination PNG
 *                                does not define
 *         ESP_ERR_NO_MEM         could not allocate the inflate buffer
 */
esp_err_t trmnl_png_render(const uint8_t *data, size_t len, epd_fb_t *fb);

/**
 * @brief  Decode an uncompressed 1-bpp BMP into @p fb, scaled to fit.
 *
 * The format the system screens use - "sign up with this Friendly ID",
 * "no plugin assigned" - rendered server-side and served before a device is
 * claimed, distinct from the PNG plugin frames P0.0 spiked against. Only
 * BITMAPINFOHEADER with a 2-entry palette is supported, which is what every
 * monochrome BMP writer produces; which palette entry is black is read from
 * the file rather than assumed, and rows may be bottom-up or top-down (BMP's
 * own sign-of-height convention) - both handled via epd_bitmap1_t rather than
 * a copy/flip pass.
 *
 * @return ESP_ERR_INVALID_ARG    not a BMP, or malformed/truncated
 *         ESP_ERR_NOT_SUPPORTED  compressed, multi-plane, or not 1 bpp
 *         ESP_ERR_INVALID_SIZE   dimensions outside what this decoder accepts
 */
esp_err_t trmnl_bmp_render(const uint8_t *data, size_t len, epd_fb_t *fb);

/**
 * @brief  Decode whatever @p data holds into @p fb, dispatching on its magic.
 *
 * PNG (plugin frames) and 1-bpp BMP (system screens) are both decoded.
 * Upstream also accepts JPEG and G5-compressed BB_BITMAP; neither has been
 * observed from the server, so rather than carry two unexercised decoders
 * this reports the format it recognised and returns ESP_ERR_NOT_SUPPORTED.
 * If one ever turns up, the log line says which.
 */
esp_err_t trmnl_image_render(const uint8_t *data, size_t len, epd_fb_t *fb);

#ifdef __cplusplus
}
#endif
