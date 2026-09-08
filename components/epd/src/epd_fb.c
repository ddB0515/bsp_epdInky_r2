#include "epd_fb.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>   /* calloc / free / abs — used by epd_fb_dither_fs */


static const char *TAG = "epd_fb";

/*******************************************************************************
 * Built-in 8×8 bitmap font — covers ASCII 0x20 (space) … 0x7E (~)
 *
 * Each character is 8 bytes, one byte per row, MSB = leftmost pixel.
 * Derived from the public-domain IBM PC BIOS 8×8 font.
 ******************************************************************************/
static const uint8_t font8x8[95][8] = {
    /* 0x20 ' ' */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 0x21 '!' */ { 0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00 },
    /* 0x22 '"' */ { 0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 0x23 '#' */ { 0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00 },
    /* 0x24 '$' */ { 0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00 },
    /* 0x25 '%' */ { 0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00 },
    /* 0x26 '&' */ { 0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00 },
    /* 0x27 '\''*/ { 0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00 },
    /* 0x28 '(' */ { 0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00 },
    /* 0x29 ')' */ { 0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00 },
    /* 0x2A '*' */ { 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00 },
    /* 0x2B '+' */ { 0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00 },
    /* 0x2C ',' */ { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06 },
    /* 0x2D '-' */ { 0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00 },
    /* 0x2E '.' */ { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00 },
    /* 0x2F '/' */ { 0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00 },
    /* 0x30 '0' */ { 0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00 },
    /* 0x31 '1' */ { 0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00 },
    /* 0x32 '2' */ { 0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00 },
    /* 0x33 '3' */ { 0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00 },
    /* 0x34 '4' */ { 0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00 },
    /* 0x35 '5' */ { 0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00 },
    /* 0x36 '6' */ { 0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00 },
    /* 0x37 '7' */ { 0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00 },
    /* 0x38 '8' */ { 0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00 },
    /* 0x39 '9' */ { 0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00 },
    /* 0x3A ':' */ { 0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00 },
    /* 0x3B ';' */ { 0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06 },
    /* 0x3C '<' */ { 0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00 },
    /* 0x3D '=' */ { 0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00 },
    /* 0x3E '>' */ { 0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00 },
    /* 0x3F '?' */ { 0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00 },
    /* 0x40 '@' */ { 0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00 },
    /* 0x41 'A' */ { 0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00 },
    /* 0x42 'B' */ { 0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00 },
    /* 0x43 'C' */ { 0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00 },
    /* 0x44 'D' */ { 0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00 },
    /* 0x45 'E' */ { 0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00 },
    /* 0x46 'F' */ { 0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00 },
    /* 0x47 'G' */ { 0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00 },
    /* 0x48 'H' */ { 0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00 },
    /* 0x49 'I' */ { 0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 0x4A 'J' */ { 0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00 },
    /* 0x4B 'K' */ { 0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00 },
    /* 0x4C 'L' */ { 0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00 },
    /* 0x4D 'M' */ { 0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00 },
    /* 0x4E 'N' */ { 0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00 },
    /* 0x4F 'O' */ { 0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00 },
    /* 0x50 'P' */ { 0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00 },
    /* 0x51 'Q' */ { 0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00 },
    /* 0x52 'R' */ { 0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00 },
    /* 0x53 'S' */ { 0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00 },
    /* 0x54 'T' */ { 0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 0x55 'U' */ { 0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00 },
    /* 0x56 'V' */ { 0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00 },
    /* 0x57 'W' */ { 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },
    /* 0x58 'X' */ { 0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00 },
    /* 0x59 'Y' */ { 0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00 },
    /* 0x5A 'Z' */ { 0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00 },
    /* 0x5B '[' */ { 0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00 },
    /* 0x5C '\'*/ { 0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00 },
    /* 0x5D ']' */ { 0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00 },
    /* 0x5E '^' */ { 0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00 },
    /* 0x5F '_' */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF },
    /* 0x60 '`' */ { 0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00 },
    /* 0x61 'a' */ { 0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00 },
    /* 0x62 'b' */ { 0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00 },
    /* 0x63 'c' */ { 0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00 },
    /* 0x64 'd' */ { 0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00 },
    /* 0x65 'e' */ { 0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00 },
    /* 0x66 'f' */ { 0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00 },
    /* 0x67 'g' */ { 0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F },
    /* 0x68 'h' */ { 0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00 },
    /* 0x69 'i' */ { 0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 0x6A 'j' */ { 0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E },
    /* 0x6B 'k' */ { 0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00 },
    /* 0x6C 'l' */ { 0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 0x6D 'm' */ { 0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00 },
    /* 0x6E 'n' */ { 0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00 },
    /* 0x6F 'o' */ { 0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00 },
    /* 0x70 'p' */ { 0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F },
    /* 0x71 'q' */ { 0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78 },
    /* 0x72 'r' */ { 0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00 },
    /* 0x73 's' */ { 0x00,0x00,0x1E,0x03,0x1E,0x30,0x1F,0x00 },
    /* 0x74 't' */ { 0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00 },
    /* 0x75 'u' */ { 0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00 },
    /* 0x76 'v' */ { 0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00 },
    /* 0x77 'w' */ { 0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00 },
    /* 0x78 'x' */ { 0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00 },
    /* 0x79 'y' */ { 0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F },
    /* 0x7A 'z' */ { 0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00 },
    /* 0x7B '{' */ { 0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00 },
    /* 0x7C '|' */ { 0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00 },
    /* 0x7D '}' */ { 0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00 },
    /* 0x7E '~' */ { 0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00 },
};

/*******************************************************************************
 * Image placement
 ******************************************************************************/

/** Read one 4bpp pixel from a packed buffer of the given pixel width. */
static inline uint8_t img4_get(const uint8_t *img, uint16_t img_w, int x, int y)
{
    const size_t stride = ((size_t)img_w + 1u) / 2u;
    const uint8_t b = img[(size_t)y * stride + (size_t)(x >> 1)];
    return (x & 1) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
}

/** Bytes per row of a 1-bpp source, honouring an explicit stride. */
static inline size_t bmp1_stride(const epd_bitmap1_t *bmp)
{
    return bmp->stride ? bmp->stride : (((size_t)bmp->width + 7u) / 8u);
}

/** Start of one 1-bpp source row, top-down regardless of storage order. */
static inline const uint8_t *bmp1_row(const epd_bitmap1_t *bmp, int y,
                                       size_t stride)
{
    const int row = bmp->bottom_up ? ((int)bmp->height - 1 - y) : y;
    return bmp->bits + (size_t)row * stride;
}

/** Read one 1-bpp pixel, expanded to a 4bpp level. */
static inline uint8_t bmp1_get(const epd_bitmap1_t *bmp, int x, int y)
{
    const uint8_t byte = bmp1_row(bmp, y, bmp1_stride(bmp))[(size_t)x >> 3];
    const bool    set  = (byte >> (7 - (x & 7))) & 1u;
    return (set != bmp->invert) ? 0x0Fu : 0x00u;
}

/*
 * A source image for the scaling blitter, in either depth.
 *
 * The rotate-and-scale loop is identical for 4bpp and 1-bpp sources; only the
 * per-pixel fetch differs.  Tagging the source here keeps one copy of that
 * loop, at the cost of one well-predicted branch per sample.
 */
typedef struct {
    const uint8_t       *pix4;   /* 4bpp packed source, or NULL for 1-bpp */
    const epd_bitmap1_t *bmp1;   /* 1-bpp source, or NULL for 4bpp        */
    uint16_t             w;
    uint16_t             h;
} blit_src_t;

static inline uint8_t src_get(const blit_src_t *s, int x, int y)
{
    return s->pix4 ? img4_get(s->pix4, s->w, x, y) : bmp1_get(s->bmp1, x, y);
}

void epd_fb_blit(epd_fb_t *fb, int x, int y,
                  const uint8_t *img, uint16_t img_w, uint16_t img_h)
{
    if (!fb || !fb->buf || !img || !img_w || !img_h) return;

    /* Clip the source rectangle against the framebuffer. */
    int sx0 = (x < 0) ? -x : 0;
    int sy0 = (y < 0) ? -y : 0;
    int sx1 = img_w, sy1 = img_h;

    if (x + sx1 > (int)fb->width)  sx1 = (int)fb->width  - x;
    if (y + sy1 > (int)fb->height) sy1 = (int)fb->height - y;
    if (sx0 >= sx1 || sy0 >= sy1) return;

    for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
            epd_fb_set_pixel(fb, (uint16_t)(x + sx), (uint16_t)(y + sy),
                             img4_get(img, img_w, sx, sy));
        }
    }
}

void epd_fb_blit_centered(epd_fb_t *fb, const uint8_t *img,
                           uint16_t img_w, uint16_t img_h, uint8_t bg)
{
    if (!fb || !fb->buf || !img || !img_w || !img_h) return;

    epd_fb_fill(fb, bg);
    epd_fb_blit(fb,
                ((int)fb->width  - (int)img_w) / 2,
                ((int)fb->height - (int)img_h) / 2,
                img, img_w, img_h);
}

/*
 * Map a coordinate in ROTATED image space back to the source image.
 *
 * Rotated space has the source's dimensions swapped for 90 and 270 degrees,
 * so callers iterate over (img_h x img_w) in those cases.  Inverting the
 * rotation here, rather than rotating pixels into a temporary buffer, means no
 * second full-size allocation.
 */
static inline void rot_to_src(epd_rotation_t rot, uint16_t img_w, uint16_t img_h,
                               int rx, int ry, int *sx, int *sy)
{
    switch (rot) {
    case EPD_ROT_90:                       /* clockwise */
        *sx = ry;
        *sy = (int)img_h - 1 - rx;
        break;
    case EPD_ROT_180:
        *sx = (int)img_w - 1 - rx;
        *sy = (int)img_h - 1 - ry;
        break;
    case EPD_ROT_270:                      /* counter-clockwise */
        *sx = (int)img_w - 1 - ry;
        *sy = rx;
        break;
    default:
        *sx = rx;
        *sy = ry;
        break;
    }
}

/** Scale factor, in 16.16, needed to fit (w x h) inside the framebuffer. */
static inline uint32_t fit_step_q16(const epd_fb_t *fb, uint32_t w, uint32_t h)
{
    const uint32_t sx = (w << 16) / fb->width;
    const uint32_t sy = (h << 16) / fb->height;
    return (sx > sy) ? sx : sy;
}

static void blit_fit_rot_src(epd_fb_t *fb, const blit_src_t *src, uint8_t bg,
                              epd_rotation_t rot)
{
    const uint16_t img_w = src->w;
    const uint16_t img_h = src->h;

    if (rot == EPD_ROT_AUTO) {
        /* Compare the on-screen area each orientation achieves. */
        const uint32_t s0 = fit_step_q16(fb, img_w, img_h);
        const uint32_t s9 = fit_step_q16(fb, img_h, img_w);
        if (!s0 || !s9) return;

        const uint64_t a0 = (uint64_t)(((uint32_t)img_w << 16) / s0) *
                            (((uint32_t)img_h << 16) / s0);
        const uint64_t a9 = (uint64_t)(((uint32_t)img_h << 16) / s9) *
                            (((uint32_t)img_w << 16) / s9);
        rot = (a9 > a0) ? EPD_ROT_90 : EPD_ROT_0;
    }

    const bool swap = (rot == EPD_ROT_90 || rot == EPD_ROT_270);
    const uint16_t ew = swap ? img_h : img_w;   /* extent in rotated space */
    const uint16_t eh = swap ? img_w : img_h;

    epd_fb_fill(fb, bg);

    const uint32_t step = fit_step_q16(fb, ew, eh);
    if (step == 0) return;

    const int dst_w = (int)(((uint32_t)ew << 16) / step);
    const int dst_h = (int)(((uint32_t)eh << 16) / step);
    if (dst_w <= 0 || dst_h <= 0) return;

    const int x0 = ((int)fb->width  - dst_w) / 2;
    const int y0 = ((int)fb->height - dst_h) / 2;

    /* Source pixels covered by one destination pixel, at least 1. */
    int box = (int)(step >> 16);
    if (box < 1) box = 1;

    for (int dy = 0; dy < dst_h; dy++) {
        const int ry_base = (int)(((uint32_t)dy * step) >> 16);

        for (int dx = 0; dx < dst_w; dx++) {
            const int rx_base = (int)(((uint32_t)dx * step) >> 16);

            /*
             * Average the source box.  When downscaling this is what preserves
             * detail that point sampling would throw away; when upscaling the
             * box is 1x1 and this degenerates to pixel repetition.
             */
            uint32_t sum = 0, n = 0;
            for (int by = 0; by < box; by++) {
                const int ry = ry_base + by;
                if (ry >= (int)eh) break;
                for (int bx = 0; bx < box; bx++) {
                    const int rx = rx_base + bx;
                    if (rx >= (int)ew) break;

                    int sx, sy;
                    rot_to_src(rot, img_w, img_h, rx, ry, &sx, &sy);
                    sum += src_get(src, sx, sy);
                    n++;
                }
            }
            if (!n) continue;

            epd_fb_set_pixel(fb, (uint16_t)(x0 + dx), (uint16_t)(y0 + dy),
                             (uint8_t)((sum + n / 2) / n));
        }
    }
}

void epd_fb_blit_fit_rot(epd_fb_t *fb, const uint8_t *img,
                          uint16_t img_w, uint16_t img_h, uint8_t bg,
                          epd_rotation_t rot)
{
    if (!fb || !fb->buf || !img || !img_w || !img_h) return;

    const blit_src_t src = { .pix4 = img, .bmp1 = NULL, .w = img_w, .h = img_h };
    blit_fit_rot_src(fb, &src, bg, rot);
}

void epd_fb_blit_fit(epd_fb_t *fb, const uint8_t *img,
                      uint16_t img_w, uint16_t img_h, uint8_t bg)
{
    epd_fb_blit_fit_rot(fb, img, img_w, img_h, bg, EPD_ROT_0);
}

/*******************************************************************************
 * 1-bpp image placement
 *
 * Everything an e-paper panel is normally asked to show - BMP files, GFX
 * bitmaps, the frames TRMNL's server renders - is one bit per pixel.  Expanding
 * that to the 4bpp framebuffer is the single hottest loop in the display path
 * (a full 1872x1404 panel is 2.6 million pixels), so the aligned case is done a
 * source byte at a time rather than through epd_fb_set_pixel().
 ******************************************************************************/

/** Expand two source bits, starting at @p hi, into one packed 4bpp byte. */
static inline uint8_t expand2(uint8_t v, int hi)
{
    return (uint8_t)(((v >> hi)       & 1u ? 0xF0u : 0x00u) |
                     ((v >> (hi - 1)) & 1u ? 0x0Fu : 0x00u));
}

void epd_fb_blit_1bpp(epd_fb_t *fb, int x, int y, const epd_bitmap1_t *bmp)
{
    if (!fb || !fb->buf || !bmp || !bmp->bits || !bmp->width || !bmp->height) {
        return;
    }

    /* Clip the source rectangle against the framebuffer, as epd_fb_blit does. */
    int sx0 = (x < 0) ? -x : 0;
    int sy0 = (y < 0) ? -y : 0;
    int sx1 = bmp->width, sy1 = bmp->height;

    if (x + sx1 > (int)fb->width)  sx1 = (int)fb->width  - x;
    if (y + sy1 > (int)fb->height) sy1 = (int)fb->height - y;
    if (sx0 >= sx1 || sy0 >= sy1) return;

    const size_t stride    = bmp1_stride(bmp);
    const size_t fb_stride = (size_t)fb->width / 2u;

    for (int sy = sy0; sy < sy1; sy++) {
        const uint8_t *src = bmp1_row(bmp, sy, stride);
        uint8_t       *dst = fb->buf + (size_t)(y + sy) * fb_stride;

        int sx = sx0;

        /*
         * Lead-in: one pixel at a time until the source sits on a byte boundary
         * and the destination on an even column.  Both hold immediately in the
         * usual full-panel case, so this loop normally does nothing.
         */
        while (sx < sx1 && ((sx & 7) != 0 || (((x + sx) & 1) != 0))) {
            const uint8_t byte = src[(size_t)sx >> 3];
            const bool    set  = (byte >> (7 - (sx & 7))) & 1u;
            epd_fb_set_pixel(fb, (uint16_t)(x + sx), (uint16_t)(y + sy),
                             (set != bmp->invert) ? 0x0Fu : 0x00u);
            sx++;
        }

        /* Body: one source byte becomes exactly four framebuffer bytes. */
        for (; sx + 8 <= sx1; sx += 8) {
            uint8_t v = src[(size_t)sx >> 3];
            if (bmp->invert) v = (uint8_t)~v;

            uint8_t *d = dst + (size_t)(x + sx) / 2u;
            d[0] = expand2(v, 7);
            d[1] = expand2(v, 5);
            d[2] = expand2(v, 3);
            d[3] = expand2(v, 1);
        }

        /* Tail: whatever the last partial byte leaves over. */
        for (; sx < sx1; sx++) {
            const uint8_t byte = src[(size_t)sx >> 3];
            const bool    set  = (byte >> (7 - (sx & 7))) & 1u;
            epd_fb_set_pixel(fb, (uint16_t)(x + sx), (uint16_t)(y + sy),
                             (set != bmp->invert) ? 0x0Fu : 0x00u);
        }
    }
}

void epd_fb_blit_1bpp_centered(epd_fb_t *fb, const epd_bitmap1_t *bmp,
                                uint8_t bg)
{
    if (!fb || !fb->buf || !bmp || !bmp->bits || !bmp->width || !bmp->height) {
        return;
    }

    epd_fb_fill(fb, bg);
    epd_fb_blit_1bpp(fb,
                     ((int)fb->width  - (int)bmp->width)  / 2,
                     ((int)fb->height - (int)bmp->height) / 2,
                     bmp);
}

void epd_fb_blit_1bpp_fit_rot(epd_fb_t *fb, const epd_bitmap1_t *bmp,
                               uint8_t bg, epd_rotation_t rot)
{
    if (!fb || !fb->buf || !bmp || !bmp->bits || !bmp->width || !bmp->height) {
        return;
    }

    /* At 1:1 the scaling path would resample for nothing, and this is the case
     * that matters: a server rendering at the panel's own resolution. */
    if (rot == EPD_ROT_0 &&
        bmp->width == fb->width && bmp->height == fb->height) {
        epd_fb_blit_1bpp(fb, 0, 0, bmp);
        return;
    }

    const blit_src_t src = {
        .pix4 = NULL, .bmp1 = bmp, .w = bmp->width, .h = bmp->height,
    };
    blit_fit_rot_src(fb, &src, bg, rot);
}

void epd_fb_blit_1bpp_fit(epd_fb_t *fb, const epd_bitmap1_t *bmp, uint8_t bg)
{
    epd_fb_blit_1bpp_fit_rot(fb, bmp, bg, EPD_ROT_0);
}

/*******************************************************************************
 * Text measurement
 ******************************************************************************/

uint16_t epd_fb_text_width_gfx(const char *text, const GFXfont *font, uint8_t scale)
{
    if (!text || !font || scale == 0) return 0;

    const GFXglyph *glyph = font->glyph;
    const uint16_t first = font->first;
    const uint16_t last  = font->last;

    uint32_t w = 0;
    for (const char *p = text; *p; p++) {
        uint16_t c = (uint8_t)*p;
        if (c < first || c > last) continue;
        w += (uint32_t)glyph[c - first].xAdvance * scale;
    }
    return (w > UINT16_MAX) ? UINT16_MAX : (uint16_t)w;
}

uint8_t epd_fb_fit_scale_gfx(const char *text, const GFXfont *font,
                              uint16_t max_width)
{
    uint16_t unit = epd_fb_text_width_gfx(text, font, 1);
    if (unit == 0) return 1;

    uint16_t s = (uint16_t)(max_width / unit);
    return (s < 1) ? 1 : (s > 255 ? 255 : (uint8_t)s);
}

uint8_t epd_fb_fit_scale(const char *text, uint16_t max_width)
{
    if (!text) return 1;

    size_t len = strlen(text);
    if (len == 0) return 1;

    uint16_t s = (uint16_t)(max_width / (len * 8u));
    return (s < 1) ? 1 : (s > 255 ? 255 : (uint8_t)s);
}

/*******************************************************************************
 * Framebuffer management
 ******************************************************************************/

esp_err_t epd_fb_create(epd_fb_t *fb, uint16_t width, uint16_t height)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    /* 4bpp packing puts two pixels per byte, so rows must be an even width. */
    if (width == 0 || height == 0 || (width & 1u)) return ESP_ERR_INVALID_ARG;

    size_t sz = ((size_t)width * height) / 2;
    fb->buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!fb->buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%zu bytes)", sz);
        return ESP_ERR_NO_MEM;
    }
    fb->width  = width;
    fb->height = height;
    memset(fb->buf, 0xFF, sz);   /* start white */
    return ESP_OK;
}

void epd_fb_destroy(epd_fb_t *fb)
{
    if (fb && fb->buf) {
        heap_caps_free(fb->buf);
        fb->buf    = NULL;
        fb->width  = 0;
        fb->height = 0;
    }
}

/*******************************************************************************
 * Drawing primitives
 ******************************************************************************/

void epd_fb_fill(epd_fb_t *fb, uint8_t level)
{
    if (!fb || !fb->buf) return;
    uint8_t byte = (uint8_t)((level & 0xF) | ((level & 0xF) << 4));
    memset(fb->buf, byte, ((size_t)fb->width * fb->height) / 2);
}

void epd_fb_set_pixel(epd_fb_t *fb, uint16_t x, uint16_t y, uint8_t level)
{
    if (!fb || !fb->buf || x >= fb->width || y >= fb->height) return;
    size_t idx = ((size_t)y * fb->width + x) / 2;
    if (x & 1) {
        fb->buf[idx] = (fb->buf[idx] & 0xF0u) | (level & 0x0Fu);
    } else {
        fb->buf[idx] = (fb->buf[idx] & 0x0Fu) | ((level & 0x0Fu) << 4);
    }
}

void epd_fb_fill_rect(epd_fb_t *fb,
                       uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint8_t  level)
{
    for (uint16_t dy = 0; dy < h; dy++) {
        for (uint16_t dx = 0; dx < w; dx++) {
            epd_fb_set_pixel(fb, x + dx, y + dy, level);
        }
    }
}

/*******************************************************************************
 * Text rendering
 ******************************************************************************/

void epd_fb_draw_char(epd_fb_t *fb,
                       uint16_t x, uint16_t y,
                       char     c,
                       uint8_t  scale,
                       uint8_t  fg,
                       uint8_t  bg)
{
    if (!fb || !fb->buf) return;
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = font8x8[(uint8_t)(c - 0x20)];

    for (uint8_t row = 0; row < 8; row++) {
        for (uint8_t col = 0; col < 8; col++) {
            bool set = (glyph[row] >> col) & 1u;
            uint8_t colour = set ? fg : bg;
            if (colour == 0xFF) continue;   /* transparent background */
            for (uint8_t sy = 0; sy < scale; sy++) {
                for (uint8_t sx = 0; sx < scale; sx++) {
                    epd_fb_set_pixel(fb,
                                     x + col * scale + sx,
                                     y + row * scale + sy,
                                     colour);
                }
            }
        }
    }
}

uint16_t epd_fb_draw_string(epd_fb_t   *fb,
                              uint16_t    x,  uint16_t y,
                              const char *str,
                              uint8_t     scale,
                              uint8_t     fg,
                              uint8_t     bg)
{
    if (!str) return x;
    while (*str) {
        epd_fb_draw_char(fb, x, y, *str, scale, fg, bg);
        x += (uint16_t)(8 * scale);
        str++;
    }
    return x;
}

/*******************************************************************************
 * GFX-font proportional text rendering (uncompressed 1bpp packed bitmaps)
 ******************************************************************************/

/* Return the 1bpp value of font pixel (row, col); out-of-bounds → 0 (bg). */
static inline int gfx_bit(const uint8_t *bitmap, uint32_t offset,
                            int row, int col, int w, int h)
{
    if (row < 0 || row >= h || col < 0 || col >= w) return 0;
    uint32_t i = (uint32_t)row * (uint32_t)w + (uint32_t)col;
    return (bitmap[offset + (i >> 3u)] >> (7u - (i & 7u))) & 1;
}

void epd_fb_draw_string_gfx(epd_fb_t      *fb,
                              int            x,
                              int            y,
                              const char    *text,
                              const GFXfont *font,
                              uint8_t        fg,
                              uint8_t        bg,
                              uint8_t        scale,
                              bool           anti_alias)
{
    if (!fb || !fb->buf || !text || !font || scale == 0) return;
    const uint8_t  *bitmap = (const uint8_t  *)font->bitmap;
    const GFXglyph *glyph  = (const GFXglyph *)font->glyph;
    uint16_t first = font->first;
    uint16_t last  = font->last;
    int s = (int)scale;

    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c < first || c > last) {
            x += (int)font->yAdvance / 2 * s;
            continue;
        }
        const GFXglyph *g = &glyph[c - first];
        if (g->width == 0 || g->height == 0) {
            x += (int)g->xAdvance * s;
            continue;
        }
        int gx = x + (int)g->xOffset * s;
        int gy = y + (int)g->yOffset * s;
        int gw = (int)g->width;
        int gh = (int)g->height;

        for (int row = 0; row < gh; row++) {
            for (int col = 0; col < gw; col++) {
                int bit = gfx_bit(bitmap, g->bitmapOffset, row, col, gw, gh);
                uint8_t colour;

                if (anti_alias) {
                    /*
                     * 4-neighbour check in font-pixel space.
                     * fg interior → 0x0  (black, 15/15 VNEG frames in GC16)
                     * fg edge     → 0x3  (dark grey, 12/15 frames, near-black)
                     * bg edge     → 0xC  (light grey, 3/15 frames, soft halo)
                     * bg far      → skip (transparent)
                     */
                    int bit_n = gfx_bit(bitmap, g->bitmapOffset, row - 1, col, gw, gh);
                    int bit_s = gfx_bit(bitmap, g->bitmapOffset, row + 1, col, gw, gh);
                    int bit_w = gfx_bit(bitmap, g->bitmapOffset, row, col - 1, gw, gh);
                    int bit_e = gfx_bit(bitmap, g->bitmapOffset, row, col + 1, gw, gh);
                    int sum = bit_n + bit_s + bit_w + bit_e;

                    if (bit) {
                        colour = (sum == 4) ? fg : 0x3u;   /* interior vs edge */
                    } else {
                        if (sum == 0) continue;             /* far from edge   */
                        colour = 0xCu;                      /* soft halo pixel */
                    }
                } else {
                    colour = bit ? fg : bg;
                    if (colour == 0xFF) continue;
                }

                /* Paint the scale×scale output block. */
                int bx = gx + col * s;
                int by = gy + row * s;
                for (int sy = 0; sy < s; sy++) {
                    int py = by + sy;
                    if (py < 0 || py >= (int)fb->height) continue;
                    for (int sx = 0; sx < s; sx++) {
                        int px = bx + sx;
                        if (px < 0 || px >= (int)fb->width) continue;
                        epd_fb_set_pixel(fb, (uint16_t)px, (uint16_t)py, colour);
                    }
                }
            }
        }
        x += (int)g->xAdvance * s;
    }
}

/*******************************************************************************
 * Floyd-Steinberg dithering
 *
 * Quantizes the 4bpp framebuffer in-place to the given palette, diffusing
 * quantization error to neighbouring pixels using the standard FS weights:
 *
 *                  current →  7/16
 *   3/16  5/16  1/16      (next row, left/centre/right)
 *
 * palette[] must contain the FB values (0–15) that map to physically distinct
 * shades on the panel.  For the 5-pass grey matrix at 26 MHz:
 *   const uint8_t pal[] = {0, 3, 6, 9, 12, 15};  // 6 physical tones
 *
 * The error buffer is heap-allocated in internal SRAM (width × 2 bytes).
 * Operates left-to-right, top-to-bottom (serpentine scan not used, keeping
 * it simple and predictable).
 ******************************************************************************/
void epd_fb_dither_fs(epd_fb_t      *fb,
                       const uint8_t *palette,
                       uint8_t        palette_size)
{
    if (!fb || !fb->buf || !palette || palette_size == 0) return;

    const uint16_t W = fb->width;
    const uint16_t H = fb->height;

    /*
     * err[1 .. W] = accumulated error for current-row-being-built (next row).
     * err[0] and err[W+1] are guard entries so we can unconditionally write
     * x-1 and x+1 without bounds checks.
     */
    int16_t *err = calloc(W + 2u, sizeof(int16_t));
    if (!err) return;
    int16_t *e = err + 1;   /* e[0..W-1] are the active entries */

    for (uint16_t y = 0; y < H; y++) {
        int16_t carry = 0;   /* 7/16 error carried rightward in this row */

        for (uint16_t x = 0; x < W; x++) {
            /* Read original pixel (0–15) and mix in accumulated errors. */
            size_t   idx  = ((size_t)y * W + x) / 2u;
            int16_t  orig = (x & 1u) ? (fb->buf[idx] & 0x0Fu)
                                      : ((fb->buf[idx] >> 4) & 0x0Fu);

            int16_t val = orig + carry + e[x];
            e[x]  = 0;              /* consume next-row error for this column */
            carry = 0;

            /* Clamp to [0, 15]. */
            if (val <  0) val = 0;
            if (val > 15) val = 15;

            /* Find nearest palette entry. */
            uint8_t nearest  = palette[0];
            int16_t min_dist = (int16_t)abs(val - (int16_t)palette[0]);
            for (int p = 1; p < palette_size; p++) {
                int16_t dist = (int16_t)abs(val - (int16_t)palette[p]);
                if (dist < min_dist) { min_dist = dist; nearest = palette[p]; }
            }

            /* Write quantized pixel back. */
            if (x & 1u) fb->buf[idx] = (fb->buf[idx] & 0xF0u) | nearest;
            else         fb->buf[idx] = (fb->buf[idx] & 0x0Fu) | (uint8_t)(nearest << 4);

            /* Distribute error with FS weights (integer approximation). */
            int16_t error = val - (int16_t)nearest;
            carry     = (int16_t)(error * 7 / 16);   /* → right      */
            e[x - 1] += (int16_t)(error * 3 / 16);   /* ↙ down-left  (guard at x=0) */
            e[x]     += (int16_t)(error * 5 / 16);   /* ↓ down       */
            e[x + 1] += (int16_t)(error * 1 / 16);   /* ↘ down-right (guard at x=W-1) */
        }
        /* carry is discarded at row end (no pixel to the right). */
    }

    free(err);
}
