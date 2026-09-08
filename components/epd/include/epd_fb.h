#ifndef EPD_FB_H
#define EPD_FB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "gfxfont.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 4bpp packed framebuffer for the EPD.
 *
 * Each byte holds two pixels:
 *   bits [7:4] = left pixel (even x)
 *   bits [3:0] = right pixel (odd x)
 *
 * Pixel values:  0x0 = black,  0xF = white
 * Buffer size:   (width * height) / 2  bytes
 * Allocation:    PSRAM via heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
 */
typedef struct {
    uint8_t  *buf;
    uint16_t  width;
    uint16_t  height;
} epd_fb_t;

/**
 * @brief  Allocate a framebuffer in PSRAM.
 * @param  fb      Pointer to epd_fb_t to initialise.
 * @param  width   Panel width in pixels.
 * @param  height  Panel height in pixels.
 */
esp_err_t epd_fb_create(epd_fb_t *fb, uint16_t width, uint16_t height);

/**
 * @brief  Free PSRAM and zero the struct.
 */
void epd_fb_destroy(epd_fb_t *fb);

/**
 * @brief  Fill entire framebuffer with one grey level (0x0–0xF).
 */
void epd_fb_fill(epd_fb_t *fb, uint8_t level);

/**
 * @brief  Set a single pixel (4-bit grey level 0x0–0xF).
 */
void epd_fb_set_pixel(epd_fb_t *fb, uint16_t x, uint16_t y, uint8_t level);

/**
 * @brief  Draw a filled rectangle.
 * @param  level  Grey level 0x0–0xF.
 */
void epd_fb_fill_rect(epd_fb_t *fb,
                       uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint8_t  level);

/**
 * @brief  Draw a single ASCII character using the built-in 8×8 font.
 *
 * @param  fb     Target framebuffer.
 * @param  x      Top-left column.
 * @param  y      Top-left row.
 * @param  c      ASCII character (printable range 0x20–0x7E).
 * @param  scale  Integer scale factor (1 = 8×8, 2 = 16×16, …).
 * @param  fg     Foreground grey level (text colour).
 * @param  bg     Background grey level (pass 0xFF to skip background).
 */
void epd_fb_draw_char(epd_fb_t *fb,
                       uint16_t x, uint16_t y,
                       char     c,
                       uint8_t  scale,
                       uint8_t  fg,
                       uint8_t  bg);

/**
 * @brief  Draw a null-terminated ASCII string.
 *
 * Characters are spaced (8 * scale) pixels apart horizontally.
 * No word-wrap is performed.
 *
 * @return  x coordinate immediately after the last character.
 */
uint16_t epd_fb_draw_string(epd_fb_t   *fb,
                              uint16_t    x,  uint16_t y,
                              const char *str,
                              uint8_t     scale,
                              uint8_t     fg,
                              uint8_t     bg);

/**
 * @brief  Draw a null-terminated string using a GFX-format proportional font.
 *
 * Bitmaps are stored 1bpp, MSB-first, not row-padded (Adafruit GFX format).
 *
 * @param  fb    Target framebuffer.
 * @param  x     Left edge of the first character (cursor x).
 * @param  y     Baseline row (glyphs render above this; descenders below).
 * @param  text  Null-terminated ASCII string to draw.
 * @param  font  Pointer to a GFXfont descriptor (Adafruit GFX-compatible).
 * @param  fg    Foreground grey level (0x0 = black). Pass 0xFF to skip.
 * @param  bg    Background grey level (0xF = white). Pass 0xFF for transparent.
 * @param  scale      Integer pixel scale factor (1 = native font size, 2 = 2×, …).
 * @param  anti_alias When true, uses 4-neighbour grey blending for smooth edges.
 *                    Requires GC16 waveform for full effect; in DU mode fg-edge
 *                    pixels (0x3) still go black (stroke slightly wider) and
 *                    bg-edge pixels (0xC) stay white.
 *                    Designed for black text on white: fg=0x0, bg=0xFF.
 */
void epd_fb_draw_string_gfx(epd_fb_t      *fb,
                              int            x,
                              int            y,
                              const char    *text,
                              const GFXfont *font,
                              uint8_t        fg,
                              uint8_t        bg,
                              uint8_t        scale,
                              bool           anti_alias);

/**
 * @brief  Width in pixels that epd_fb_draw_string_gfx() would occupy.
 *
 * Sums each glyph's xAdvance, so it accounts for the proportional spacing of a
 * GFX font.  Characters outside the font's range are skipped, matching what the
 * draw function does.  Use this to centre text or to pick a scale that fits.
 *
 * @return Width in pixels, or 0 for empty or unusable arguments.
 */
uint16_t epd_fb_text_width_gfx(const char *text, const GFXfont *font, uint8_t scale);

/**
 * @brief  Largest integer scale at which @p text fits within @p max_width.
 *
 * @return A scale of at least 1, even when the text cannot be made to fit.
 */
uint8_t epd_fb_fit_scale_gfx(const char *text, const GFXfont *font,
                              uint16_t max_width);

/**
 * @brief  Largest integer scale at which @p text fits using the built-in 8x8
 *         font, whose glyphs are a fixed 8*scale wide.
 *
 * @return A scale of at least 1, even when the text cannot be made to fit.
 */
uint8_t epd_fb_fit_scale(const char *text, uint16_t max_width);

/**
 * @brief  Draw a 4bpp packed image into the framebuffer at (@p x, @p y).
 *
 * The image uses the same packing as the framebuffer: two pixels per byte,
 * high nibble first, so its rows are (@p img_w + 1) / 2 bytes.  Anything
 * falling outside the framebuffer is clipped, and @p x / @p y may be negative
 * to show only part of a large image.
 *
 * @param img    4bpp packed source pixels.
 * @param img_w  Source width in pixels.
 * @param img_h  Source height in pixels.
 */
void epd_fb_blit(epd_fb_t *fb, int x, int y,
                  const uint8_t *img, uint16_t img_w, uint16_t img_h);

/**
 * @brief  Draw a 4bpp image centred, at its native size.
 *
 * The surrounding area is filled with @p bg first.  An image larger than the
 * panel is cropped equally on all sides rather than scaled.
 */
void epd_fb_blit_centered(epd_fb_t *fb, const uint8_t *img,
                           uint16_t img_w, uint16_t img_h, uint8_t bg);

/** Image rotation, applied before the image is scaled and centred. */
typedef enum {
    EPD_ROT_0   = 0,   /**< no rotation                                      */
    EPD_ROT_90  = 1,   /**< 90 degrees clockwise                             */
    EPD_ROT_180 = 2,   /**< upside down                                      */
    EPD_ROT_270 = 3,   /**< 90 degrees counter-clockwise                     */
    EPD_ROT_AUTO = 4,  /**< whichever of 0 / 90 covers more of the panel     */
} epd_rotation_t;

/**
 * @brief  Rotate and scale a 4bpp image to fit the panel, preserving aspect.
 *
 * Rotation happens first, so a portrait image on a landscape panel can be
 * turned to use the full screen instead of being letterboxed into a narrow
 * column.  EPD_ROT_AUTO measures both orientations and picks the one that
 * covers more of the panel, which is usually what is wanted.
 *
 * @param bg   Grey level for the letterbox margins.
 * @param rot  Rotation to apply, or EPD_ROT_AUTO to choose automatically.
 */
void epd_fb_blit_fit_rot(epd_fb_t *fb, const uint8_t *img,
                          uint16_t img_w, uint16_t img_h, uint8_t bg,
                          epd_rotation_t rot);

/**
 * @brief  Scale a 4bpp image to fit the panel, preserving aspect ratio.
 *
 * The image is scaled by the largest factor that keeps it fully on screen and
 * centred, with @p bg filling the letterbox margins.  This is what lets one
 * generated image be shown on panels of different sizes.
 *
 * Downscaling averages over the source region each output pixel covers, which
 * keeps detail that point sampling would drop.  Upscaling repeats pixels, so
 * enlarging a small image looks blocky - prefer regenerating the image at the
 * target size with convert_image.py, which resizes and dithers with far better
 * quality than is worth doing on the device.
 */
void epd_fb_blit_fit(epd_fb_t *fb, const uint8_t *img,
                      uint16_t img_w, uint16_t img_h, uint8_t bg);

/**
 * A 1-bpp source image.
 *
 * This is the shape almost every e-paper image arrives in: BMP files, Adafruit
 * GFX bitmaps, and the frames TRMNL's server renders are all one bit per pixel,
 * MSB-first within each byte (bit 7 is the leftmost pixel of the group).
 *
 * @c stride and @c bottom_up exist so an ordinary BMP can be blitted straight
 * out of the buffer it was downloaded into, with no intermediate copy: BMP pads
 * every row to a 4-byte boundary and stores the last row first.
 */
typedef struct {
    const uint8_t *bits;       /**< Packed source, MSB-first within each byte. */
    uint16_t       width;      /**< Width in pixels.                           */
    uint16_t       height;     /**< Height in pixels.                          */
    size_t         stride;     /**< Bytes per row; 0 selects (width + 7) / 8.  */
    bool           bottom_up;  /**< Rows stored last-first, as in a BMP.       */
    bool           invert;     /**< Make a set bit black instead of white.     */
} epd_bitmap1_t;

/**
 * @brief  Draw a 1-bpp image into the framebuffer at (@p x, @p y).
 *
 * Each source bit is expanded to a full 4bpp pixel: by default a set bit
 * becomes white (0xF) and a clear bit black (0x0), which is how a 1-bpp BMP
 * with the usual palette reads.  Set @c invert when the source uses the
 * opposite convention.
 *
 * Clipping matches epd_fb_blit(): anything outside the framebuffer is dropped
 * and @p x / @p y may be negative.  When the destination lands on an even
 * column the inner loop expands whole source bytes into four framebuffer bytes
 * at a time, so a full-panel image costs one pass over the source.
 *
 * The result contains only 0x0 and 0xF, so it can be pushed with
 * EPD_WAVEFORM_DU as well as GC16 - DU is several times faster and loses
 * nothing on an image that has no intermediate greys to begin with.
 */
void epd_fb_blit_1bpp(epd_fb_t *fb, int x, int y, const epd_bitmap1_t *bmp);

/**
 * @brief  Draw a 1-bpp image centred, at its native size.
 *
 * The surrounding area is filled with @p bg first.  An image larger than the
 * panel is cropped equally on all sides rather than scaled.
 */
void epd_fb_blit_1bpp_centered(epd_fb_t *fb, const epd_bitmap1_t *bmp,
                                uint8_t bg);

/**
 * @brief  Rotate and scale a 1-bpp image to fit the panel, preserving aspect.
 *
 * The 1-bpp counterpart of epd_fb_blit_fit_rot(), with the same semantics.
 *
 * Note that only the 1:1 and upscaled cases stay purely black and white.
 * Downscaling averages over the source box, so edges come out grey - fine
 * under GC16, and the reason to prefer having the server render at the panel's
 * own resolution.
 *
 * @param bg   Grey level for the letterbox margins.
 * @param rot  Rotation to apply, or EPD_ROT_AUTO to choose automatically.
 */
void epd_fb_blit_1bpp_fit_rot(epd_fb_t *fb, const epd_bitmap1_t *bmp,
                               uint8_t bg, epd_rotation_t rot);

/**
 * @brief  Scale a 1-bpp image to fit the panel, preserving aspect ratio.
 */
void epd_fb_blit_1bpp_fit(epd_fb_t *fb, const epd_bitmap1_t *bmp, uint8_t bg);

/**
 * @brief  Floyd-Steinberg dithering — quantize framebuffer to a palette.
 *
 * Processes the 4bpp framebuffer in-place, left-to-right top-to-bottom.
 * Each pixel is rounded to the nearest entry in @p palette and the
 * quantization error is diffused to the right and down neighbours using
 * standard FS weights (7/16, 3/16, 5/16, 1/16).
 *
 * Use this before EPD_WAVEFORM_GC16 to make 6 physical tones appear as a
 * smooth 16-level gradient.  For the 5-pass 26 MHz grey matrix:
 *
 *   static const uint8_t pal[] = {0, 3, 6, 9, 12, 15};
 *   epd_fb_dither_fs(&fb, pal, 6);
 *
 * @param  fb            4bpp framebuffer to dither in-place.
 * @param  palette       Array of 4bpp grey values (0–15) that map to
 *                       physically distinct shades on the panel.
 *                       Must be sorted from darkest (0) to lightest (15).
 * @param  palette_size  Number of entries in @p palette.
 */
void epd_fb_dither_fs(epd_fb_t      *fb,
                       const uint8_t *palette,
                       uint8_t        palette_size);

#ifdef __cplusplus
}
#endif

#endif /* EPD_FB_H */
