#include "ha_image.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * Inflate lives in the ESP32-P4 boot ROM. esp_rom exports tinfl_decompress and
 * friends (esp32p4.rom.ld), and the matching header ships with the component,
 * so this costs nothing in flash or dependencies.
 */
#include "miniz.h"

static const char *TAG = "ha_image";

/* -------------------------------------------------------------------------- */
/* Limits                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * Nothing here trusts the network. An image comes from a user-configured URL
 * over a connection this firmware does not otherwise control, so every size
 * is bounded before it reaches an allocator or an index.
 *
 * 4096 covers the panel (1872x1404) with room for a server-side change of mind;
 * 8 MB bounds the inflated scanline buffer, which for the real frames is 1.29 MB
 * and for anything plausible stays well under the cap.
 */
#define PNG_MAX_DIM         4096u
#define PNG_MAX_RAW_BYTES   (8u * 1024u * 1024u)

/* -------------------------------------------------------------------------- */
/* Byte-level helpers                                                         */
/* -------------------------------------------------------------------------- */

static const uint8_t k_png_magic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

#define PNG_FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define PNG_IHDR  PNG_FOURCC('I', 'H', 'D', 'R')
#define PNG_PLTE  PNG_FOURCC('P', 'L', 'T', 'E')
#define PNG_IDAT  PNG_FOURCC('I', 'D', 'A', 'T')
#define PNG_IEND  PNG_FOURCC('I', 'E', 'N', 'D')

static inline uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Cursor over a PNG's chunk sequence. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} png_chunks_t;

/**
 * Advance to the next chunk. Returns false at IEND, at the end of the buffer, or
 * on any length that would run past it - a truncated download therefore stops
 * the walk instead of reading out of bounds.
 *
 * CRCs are not checked. The zlib stream carries an Adler-32 that tinfl verifies,
 * and the transport underneath is TCP over TLS; a per-chunk CRC would only catch
 * a corruption those two already exclude.
 */
static bool png_chunk_next(png_chunks_t *it, uint32_t *type, const uint8_t **data, uint32_t *len)
{
    if ((size_t)(it->end - it->p) < 12u) {
        return false;
    }

    const uint32_t n = rd_be32(it->p);
    if (n > (size_t)(it->end - it->p) - 12u) {
        return false;   /* length runs past the buffer */
    }

    *len  = n;
    *type = rd_be32(it->p + 4);
    *data = it->p + 8;
    it->p += 12u + n;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Header                                                                     */
/* -------------------------------------------------------------------------- */

/** Samples per pixel for a PNG colour type, or 0 if the type is not defined. */
static uint8_t png_samples(uint8_t colour_type)
{
    switch (colour_type) {
    case 0: return 1;   /* grey */
    case 2: return 3;   /* RGB */
    case 3: return 1;   /* palette index */
    case 4: return 2;   /* grey + alpha */
    case 6: return 4;   /* RGBA */
    default: return 0;
    }
}

/** Is this a bit depth the spec allows for this colour type? */
static bool png_depth_ok(uint8_t colour_type, uint8_t bit_depth)
{
    switch (colour_type) {
    case 0:  return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                    bit_depth == 8 || bit_depth == 16;
    case 3:  return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
    case 2:
    case 4:
    case 6:  return bit_depth == 8 || bit_depth == 16;
    default: return false;
    }
}

esp_err_t ha_png_probe(const uint8_t *data, size_t len, ha_png_info_t *out)
{
    if (data == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* signature + IHDR chunk (4 len + 4 type + 13 body + 4 crc) */
    if (len < sizeof(k_png_magic) + 25u || memcmp(data, k_png_magic, sizeof(k_png_magic)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    png_chunks_t it = { .p = data + sizeof(k_png_magic), .end = data + len };

    uint32_t       type = 0, clen = 0;
    const uint8_t *body = NULL;
    if (!png_chunk_next(&it, &type, &body, &clen) || type != PNG_IHDR || clen != 13u) {
        return ESP_ERR_INVALID_ARG;
    }

    out->width       = rd_be32(body);
    out->height      = rd_be32(body + 4);
    out->bit_depth   = body[8];
    out->colour_type = body[9];
    out->interlaced  = body[12] != 0;

    if (out->width == 0 || out->height == 0 ||
        out->width > PNG_MAX_DIM || out->height > PNG_MAX_DIM) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Sample extraction and grey reduction                                       */
/* -------------------------------------------------------------------------- */

/**
 * Sample @p i of a scanline, as a value in the natural range of @p bd.
 *
 * For 16-bit depths only the high byte is returned. The panel has 16 levels; the
 * low byte cannot survive the reduction and reading it would only cost cycles.
 */
static inline uint32_t sample_at(const uint8_t *row, uint8_t bd, uint32_t i)
{
    switch (bd) {
    case 8:  return row[i];
    case 4:  return (row[i >> 1] >> ((i & 1u) ? 0 : 4)) & 0x0Fu;
    case 2:  return (row[i >> 2] >> (6u - 2u * (i & 3u))) & 0x03u;
    case 1:  return (row[i >> 3] >> (7u - (i & 7u))) & 0x01u;
    case 16: return row[(size_t)i * 2u];
    default: return 0;
    }
}

/** Grey sample of depth @p bd to one of the panel's 16 levels (0 black, 15 white). */
static inline uint8_t grey_to_level(uint32_t v, uint8_t bd)
{
    switch (bd) {
    case 1:  return (uint8_t)(v * 15u);         /* 0,15 */
    case 2:  return (uint8_t)(v * 5u);          /* 0,5,10,15 */
    case 4:  return (uint8_t)v;
    default: return (uint8_t)(v >> 4);          /* 8 and 16 both arrive as 0..255 */
    }
}

/** Rec. 601 luminance of an 8-bit RGB triple. */
static inline uint32_t rgb_luma(uint32_t r, uint32_t g, uint32_t b)
{
    return (77u * r + 150u * g + 29u * b) >> 8;
}

/** Composite an 8-bit value over a white background at 8-bit alpha. */
static inline uint32_t over_white(uint32_t v, uint32_t a)
{
    return (v * a + 255u * (255u - a) + 127u) / 255u;
}

/* -------------------------------------------------------------------------- */
/* Decoder state                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  bit_depth;
    uint8_t  colour_type;
    uint8_t  samples;
    size_t   src_stride;    /**< bytes per filtered scanline, excluding the filter byte */
    size_t   dst_stride;    /**< bytes per 4bpp output row */
    uint8_t  filter_bpp;    /**< filter offset to the pixel to the left, in bytes */

    bool     have_plte;
    uint8_t  pal_level[256];    /**< palette index -> panel level */
    uint8_t  pal_lut16[256];    /**< 4bpp fast path: two indices -> two levels, in one step */
} png_dec_t;

/** One pixel of a scanline, reduced to a panel level. */
static uint8_t pixel_level(const uint8_t *row, uint32_t x, const png_dec_t *d)
{
    const uint8_t bd = d->bit_depth;

    switch (d->colour_type) {
    case 0:
        return grey_to_level(sample_at(row, bd, x), bd);

    case 3:
        return d->pal_level[sample_at(row, bd, x) & 0xFFu];

    case 4: {
        const uint32_t g = sample_at(row, bd, x * 2u);
        const uint32_t a = sample_at(row, bd, x * 2u + 1u);
        return (uint8_t)(over_white(g, a) >> 4);
    }

    case 2: {
        const uint32_t i = x * 3u;
        const uint32_t y = rgb_luma(sample_at(row, bd, i),
                                    sample_at(row, bd, i + 1u),
                                    sample_at(row, bd, i + 2u));
        return (uint8_t)(y >> 4);
    }

    case 6: {
        const uint32_t i = x * 4u;
        const uint32_t y = rgb_luma(sample_at(row, bd, i),
                                    sample_at(row, bd, i + 1u),
                                    sample_at(row, bd, i + 2u));
        return (uint8_t)(over_white(y, sample_at(row, bd, i + 3u)) >> 4);
    }

    default:
        return 0x0F;
    }
}

/**
 * Convert one unfiltered scanline into a 4bpp row, leftmost pixel in the high
 * nibble.
 *
 * Two shapes get a fast path because they are the two the server actually sends:
 *
 *   grey, 4bpp     PNG defines sample v as v/15 white, which is epd_fb's own
 *                  encoding, and the packing order matches - so the row is
 *                  already in its final form and this is a memcpy.
 *   indexed, 4bpp  both nibbles convert at once through a 256-entry table built
 *                  from PLTE, which is a byte-for-byte pass over the row.
 *
 * Everything else goes per pixel.
 */
static void row_to_grey4(uint8_t *dst, const uint8_t *src, const png_dec_t *d)
{
    if (d->bit_depth == 4 && d->colour_type == 0) {
        memcpy(dst, src, d->dst_stride);
        return;
    }
    if (d->bit_depth == 4 && d->colour_type == 3) {
        for (size_t i = 0; i < d->dst_stride; i++) {
            dst[i] = d->pal_lut16[src[i]];
        }
        return;
    }

    uint8_t *o   = dst;
    uint8_t  acc = 0;
    for (uint32_t x = 0; x < d->width; x++) {
        const uint8_t lvl = pixel_level(src, x, d);
        if ((x & 1u) == 0u) {
            acc = (uint8_t)(lvl << 4);
        } else {
            *o++ = (uint8_t)(acc | lvl);
        }
    }
    if (d->width & 1u) {
        /* Odd width: the low nibble is off the right edge. Mirror the last
         * pixel rather than leaving it black, so a stray column cannot appear. */
        *o = (uint8_t)(acc | (acc >> 4));
    }
}

/* -------------------------------------------------------------------------- */
/* Row filters                                                                */
/* -------------------------------------------------------------------------- */

static inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    const int p  = (int)a + (int)b - (int)c;
    const int pa = p > (int)a ? p - (int)a : (int)a - p;
    const int pb = p > (int)b ? p - (int)b : (int)b - p;
    const int pc = p > (int)c ? p - (int)c : (int)c - p;

    if (pa <= pb && pa <= pc) {
        return a;
    }
    return (pb <= pc) ? b : c;
}

/**
 * Undo the per-row filters in place over the inflated buffer.
 *
 * Each row is (1 + src_stride) bytes: a filter byte then the data. Rows are
 * processed in order, so by the time row y reads row y-1 that row is already
 * reconstructed - which is what the format requires.
 */
static esp_err_t png_unfilter(uint8_t *raw, const png_dec_t *d)
{
    const size_t  rowlen = d->src_stride + 1u;
    const size_t  bpp    = d->filter_bpp;
    const uint8_t *prev  = NULL;

    for (uint32_t y = 0; y < d->height; y++) {
        uint8_t      *row  = raw + (size_t)y * rowlen;
        const uint8_t ft   = row[0];
        uint8_t      *cur  = row + 1;

        switch (ft) {
        case 0:     /* None */
            break;

        case 1:     /* Sub */
            for (size_t i = bpp; i < d->src_stride; i++) {
                cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
            }
            break;

        case 2:     /* Up */
            if (prev != NULL) {
                for (size_t i = 0; i < d->src_stride; i++) {
                    cur[i] = (uint8_t)(cur[i] + prev[i]);
                }
            }
            break;

        case 3:     /* Average */
            for (size_t i = 0; i < d->src_stride; i++) {
                const uint32_t a = (i >= bpp) ? cur[i - bpp] : 0u;
                const uint32_t b = (prev != NULL) ? prev[i] : 0u;
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;

        case 4:     /* Paeth */
            for (size_t i = 0; i < d->src_stride; i++) {
                const uint8_t a = (i >= bpp) ? cur[i - bpp] : 0u;
                const uint8_t b = (prev != NULL) ? prev[i] : 0u;
                const uint8_t c = (prev != NULL && i >= bpp) ? prev[i - bpp] : 0u;
                cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
            }
            break;

        default:
            ESP_LOGE(TAG, "row %" PRIu32 ": unknown filter type %u", y, ft);
            return ESP_ERR_INVALID_ARG;
        }

        prev = cur;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Inflate                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Inflate every IDAT chunk into @p raw, which must be exactly the expected size.
 *
 * The IDATs form one zlib stream that happens to be cut into chunks, so the
 * decompressor is fed each chunk in turn with TINFL_FLAG_HAS_MORE_INPUT set on
 * all but the last. That avoids concatenating the compressed data first, which
 * for a 14 KB image would be cheap but for a large one would not.
 *
 * TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF is safe because the output buffer is
 * the whole image: back-references can never reach behind its start.
 */
static esp_err_t png_inflate_idat(const uint8_t *data, size_t len,
                                  uint8_t *raw, size_t raw_size,
                                  const uint8_t **plte_out, uint32_t *plte_len)
{
    tinfl_decompressor *decomp = heap_caps_malloc(sizeof(*decomp), MALLOC_CAP_DEFAULT);
    if (decomp == NULL) {
        /* ~11 KB. Internal RAM is preferred - it is touched constantly - but
         * PSRAM works and is better than failing the render. */
        decomp = heap_caps_malloc(sizeof(*decomp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (decomp == NULL) {
        ESP_LOGE(TAG, "no memory for the inflate state (%u bytes)", (unsigned)sizeof(*decomp));
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(decomp);

    uint8_t *out           = raw;
    size_t   out_remaining = raw_size;
    bool     done          = false;
    esp_err_t err          = ESP_OK;

    /*
     * One pass. PLTE is required to precede the first IDAT, so capturing it here
     * rather than in a separate walk is correct and saves traversing twice.
     */
    png_chunks_t it = { .p = data + sizeof(k_png_magic), .end = data + len };

    uint32_t       type = 0, clen = 0;
    const uint8_t *body = NULL;

    while (!done && png_chunk_next(&it, &type, &body, &clen)) {
        if (type == PNG_PLTE) {
            *plte_out = body;
            *plte_len = clen;
            continue;
        }
        if (type == PNG_IEND) {
            break;
        }
        if (type != PNG_IDAT) {
            continue;
        }

        /*
         * Is this the last IDAT? Peek ahead: if another one follows, the stream
         * continues and tinfl must not treat the end of this chunk as the end of
         * input. Peeking costs a second walk of the chunk headers, which is a
         * handful of pointer additions.
         */
        bool more_idat = false;
        {
            png_chunks_t look = it;
            uint32_t       t2 = 0, l2 = 0;
            const uint8_t *b2 = NULL;
            while (png_chunk_next(&look, &t2, &b2, &l2)) {
                if (t2 == PNG_IDAT) { more_idat = true; break; }
                if (t2 == PNG_IEND) { break; }
            }
        }

        const uint8_t *in           = body;
        size_t         in_remaining = clen;

        for (;;) {
            size_t in_bytes  = in_remaining;
            size_t out_bytes = out_remaining;

            mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
                              TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
            if (more_idat) {
                flags |= TINFL_FLAG_HAS_MORE_INPUT;
            }

            const tinfl_status st = tinfl_decompress(decomp, in, &in_bytes,
                                                     raw, out, &out_bytes, flags);
            in            += in_bytes;
            in_remaining  -= in_bytes;
            out           += out_bytes;
            out_remaining -= out_bytes;

            if (st < 0) {
                ESP_LOGE(TAG, "inflate failed: tinfl status %d", (int)st);
                err = ESP_ERR_INVALID_ARG;
                goto out;
            }
            if (st == TINFL_STATUS_DONE) {
                done = true;
                break;
            }
            if (out_remaining == 0u) {
                /*
                 * Every scanline byte the header calls for is present, but the
                 * decompressor has not reached the end of the stream - it still
                 * has a block header or the trailing Adler-32 to parse, and no
                 * room to write into. There is nothing more to extract, so stop
                 * here rather than spin.
                 *
                 * This is the normal outcome when the image arrives as several
                 * IDAT chunks: the pixel data finishes inside one of them and
                 * the end marker lands in the next. The Adler-32 goes unchecked
                 * on that path; a short or corrupt stream is still caught by the
                 * length test below, and TLS already rules out the corruption a
                 * checksum would find.
                 */
                done = true;
                break;
            }
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT) {
                break;      /* on to the next IDAT */
            }
            /* HAS_MORE_OUTPUT with room left: keep going. */
        }
    }

    if (out_remaining != 0u) {
        ESP_LOGE(TAG, "inflated %u bytes, expected %u",
                 (unsigned)(raw_size - out_remaining), (unsigned)raw_size);
        err = ESP_ERR_INVALID_SIZE;
    }

out:
    heap_caps_free(decomp);
    return err;
}

/* -------------------------------------------------------------------------- */
/* Palette                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Build the index -> level table from PLTE.
 *
 * The system screens use a linear ramp from white at index 0 to black at 15, so
 * for those this comes out as `index ^ 0x0F`. Nothing depends on that: the table
 * is derived from the actual entries, so a palette in any order works.
 *
 * Indices beyond PLTE's length are not legal but are mapped to white rather than
 * left undefined, so a malformed image renders blank instead of as noise.
 */
static void png_build_palette(png_dec_t *d, const uint8_t *plte, uint32_t plte_len)
{
    const uint32_t entries = plte_len / 3u;

    for (uint32_t i = 0; i < 256u; i++) {
        if (i < entries) {
            const uint32_t y = rgb_luma(plte[i * 3u], plte[i * 3u + 1u], plte[i * 3u + 2u]);
            d->pal_level[i] = (uint8_t)(y >> 4);
        } else {
            d->pal_level[i] = 0x0F;
        }
    }

    /* Both nibbles of a packed byte, resolved in one lookup. */
    for (uint32_t b = 0; b < 256u; b++) {
        d->pal_lut16[b] = (uint8_t)((d->pal_level[b >> 4] << 4) | d->pal_level[b & 0x0Fu]);
    }
}

/* -------------------------------------------------------------------------- */
/* Public entry points                                                        */
/* -------------------------------------------------------------------------- */

esp_err_t ha_png_render(const uint8_t *data, size_t len, epd_fb_t *fb)
{
    if (fb == NULL || fb->buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ha_png_info_t info;
    esp_err_t err = ha_png_probe(data, len, &info);
    if (err != ESP_OK) {
        return err;
    }

    if (info.interlaced) {
        ESP_LOGE(TAG, "interlaced (Adam7) PNG is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!png_depth_ok(info.colour_type, info.bit_depth)) {
        ESP_LOGE(TAG, "colour type %u at bit depth %u is not a valid combination",
                 info.colour_type, info.bit_depth);
        return ESP_ERR_NOT_SUPPORTED;
    }

    png_dec_t d = {
        .width       = info.width,
        .height      = info.height,
        .bit_depth   = info.bit_depth,
        .colour_type = info.colour_type,
        .samples     = png_samples(info.colour_type),
    };
    d.src_stride = ((size_t)d.width * d.samples * d.bit_depth + 7u) / 8u;
    d.dst_stride = ((size_t)d.width + 1u) / 2u;
    d.filter_bpp = (uint8_t)(((uint32_t)d.samples * d.bit_depth + 7u) / 8u);

    const size_t raw_size = (size_t)d.height * (d.src_stride + 1u);
    if (raw_size > PNG_MAX_RAW_BYTES) {
        ESP_LOGE(TAG, "image would inflate to %u bytes, over the %u cap",
                 (unsigned)raw_size, (unsigned)PNG_MAX_RAW_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "PNG %" PRIu32 "x%" PRIu32 ", %u bpp, colour type %u -> %u bytes of scanlines",
             d.width, d.height, d.bit_depth, d.colour_type, (unsigned)raw_size);

    uint8_t *raw = heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        raw = heap_caps_malloc(raw_size, MALLOC_CAP_DEFAULT);
    }
    if (raw == NULL) {
        ESP_LOGE(TAG, "no memory for %u bytes of scanlines", (unsigned)raw_size);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *scratch = NULL;

    const uint8_t *plte     = NULL;
    uint32_t       plte_len = 0;

    err = png_inflate_idat(data, len, raw, raw_size, &plte, &plte_len);
    if (err != ESP_OK) {
        goto done;
    }

    if (d.colour_type == 3) {
        if (plte == NULL || plte_len < 3u || (plte_len % 3u) != 0u) {
            ESP_LOGE(TAG, "indexed image with no usable PLTE (%" PRIu32 " bytes)", plte_len);
            err = ESP_ERR_INVALID_ARG;
            goto done;
        }
        png_build_palette(&d, plte, plte_len);
        d.have_plte = true;
    }

    err = png_unfilter(raw, &d);
    if (err != ESP_OK) {
        goto done;
    }

    /*
     * At the panel's own size the rows go straight into the framebuffer. This is
     * the case that actually happens - claiming Model "x" makes the server
     * render at 1872x1404 - and it avoids a second 1.3 MB buffer and a rescale.
     */
    const bool native = (d.width == fb->width) && (d.height == fb->height);

    uint8_t *out_base;
    size_t   out_stride;

    if (native) {
        out_base   = fb->buf;
        out_stride = (size_t)fb->width / 2u;    /* epd_fb's own convention */
    } else {
        out_stride = d.dst_stride;
        scratch = heap_caps_malloc((size_t)d.height * out_stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (scratch == NULL) {
            scratch = heap_caps_malloc((size_t)d.height * out_stride, MALLOC_CAP_DEFAULT);
        }
        if (scratch == NULL) {
            ESP_LOGE(TAG, "no memory to rescale a %" PRIu32 "x%" PRIu32 " image", d.width, d.height);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        out_base = scratch;
    }

    for (uint32_t y = 0; y < d.height; y++) {
        const uint8_t *src = raw + (size_t)y * (d.src_stride + 1u) + 1u;
        row_to_grey4(out_base + (size_t)y * out_stride, src, &d);
    }

    if (!native) {
        ESP_LOGW(TAG, "image is %" PRIu32 "x%" PRIu32 " but the panel is %ux%u; fitting",
                 d.width, d.height, fb->width, fb->height);
        /* 0x0F letterboxes in white, which is what the panel reads as blank. */
        epd_fb_blit_fit_rot(fb, out_base, (uint16_t)d.width, (uint16_t)d.height,
                            0x0F, EPD_ROT_AUTO);
    }

done:
    heap_caps_free(scratch);
    heap_caps_free(raw);
    return err;
}

/* -------------------------------------------------------------------------- */
/* BMP - system screens only ("setup-logo.bmp" etc.); plugin frames are PNG   */
/* -------------------------------------------------------------------------- */

static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

static inline uint32_t rd_le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

/*
 * A convenience path for render services that default to monochrome BMP
 * rather than PNG. Just the one variant matters: an uncompressed 1-bpp
 * BITMAPINFOHEADER with a 2-entry palette, which is what every monochrome BMP
 * writer produces. Nothing here trusts the network any more than the PNG path
 * does - every offset is bounds-checked against the actual body length before
 * use.
 */
esp_err_t ha_bmp_render(const uint8_t *data, size_t len, epd_fb_t *fb)
{
    /* BITMAPFILEHEADER (14) + the smallest BITMAPINFOHEADER (40). */
    if (len < 54u) {
        ESP_LOGE(TAG, "BMP: %u bytes is too short for a header", (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t data_offset     = rd_le32(data + 10);
    const uint32_t dib_header_size = rd_le32(data + 14);
    if (dib_header_size < 40u || (uint64_t)14 + dib_header_size > (uint64_t)len) {
        ESP_LOGE(TAG, "BMP: unsupported or truncated DIB header (%" PRIu32 " bytes)",
                 dib_header_size);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int32_t  width      = (int32_t)rd_le32(data + 18);
    const int32_t  height_raw = (int32_t)rd_le32(data + 22);
    const uint16_t planes     = rd_le16(data + 26);
    const uint16_t bpp        = rd_le16(data + 28);
    const uint32_t compression = rd_le32(data + 30);

    if (planes != 1u || bpp != 1u || compression != 0u) {
        ESP_LOGE(TAG, "BMP: only uncompressed 1bpp is supported (got %u plane(s), "
                      "%u bpp, compression %" PRIu32 ")", planes, bpp, compression);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (width <= 0 || width > (int32_t)PNG_MAX_DIM || height_raw == 0 ||
        (height_raw > 0 ? height_raw : -height_raw) > (int32_t)PNG_MAX_DIM) {
        ESP_LOGE(TAG, "BMP: implausible %" PRId32 "x%" PRId32, width, height_raw);
        return ESP_ERR_INVALID_SIZE;
    }

    /* A positive height is BMP's own default bottom-up row order; negative
     * means top-down. epd_bitmap1_t already has a field for exactly this, so
     * there is nothing to flip here - just record it. */
    const bool     bottom_up = (height_raw > 0);
    const uint32_t height    = (uint32_t)(bottom_up ? height_raw : -height_raw);

    /* The palette sits right after the DIB header: 2 entries, 4 bytes each
     * (B, G, R, reserved). Read entry 0 to learn which bit value the file
     * calls black, since epd_fb_blit_1bpp() only knows "set bit = ink or
     * background", not an arbitrary palette. */
    const uint8_t *palette = data + 14 + dib_header_size;
    if ((uint64_t)(palette - data) + 8u > (uint64_t)len) {
        ESP_LOGE(TAG, "BMP: truncated before the palette");
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t luma0 = (uint32_t)palette[0] + palette[1] + palette[2];
    const uint32_t luma1 = (uint32_t)palette[4] + palette[5] + palette[6];
    /* epd_fb_blit_1bpp()'s default is "set bit -> white"; invert whenever a
     * *clear* bit is the darker palette entry, so a set bit ends up black. */
    const bool invert = luma1 < luma0;

    /* BMP rows are padded to a 4-byte boundary - exactly what
     * epd_bitmap1_t.stride exists for. */
    const size_t stride = (((size_t)width + 31u) / 32u) * 4u;
    const uint64_t pixel_bytes = (uint64_t)stride * height;
    if ((uint64_t)data_offset + pixel_bytes > (uint64_t)len) {
        ESP_LOGE(TAG, "BMP: pixel data (%" PRIu64 " bytes from offset %" PRIu32
                      ") exceeds the %u-byte body", pixel_bytes, data_offset, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    const epd_bitmap1_t bmp = {
        .bits      = data + data_offset,
        .width     = (uint16_t)width,
        .height    = (uint16_t)height,
        .stride    = stride,
        .bottom_up = bottom_up,
        .invert    = invert,
    };

    ESP_LOGI(TAG, "BMP %" PRIu32 "x%u, 1bpp, %s", (uint32_t)width, height,
             bottom_up ? "bottom-up" : "top-down");

    epd_fb_fill(fb, 0x0F);
    epd_fb_blit_1bpp_fit(fb, &bmp, 0x0F);
    return ESP_OK;
}

esp_err_t ha_image_render(const uint8_t *data, size_t len, epd_fb_t *fb)
{
    if (data == NULL || len < 8u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (memcmp(data, k_png_magic, sizeof(k_png_magic)) == 0) {
        return ha_png_render(data, len, fb);
    }
    if (data[0] == 'B' && data[1] == 'M') {
        return ha_bmp_render(data, len, fb);
    }

    /*
     * A format this decoder does not implement. Naming it in the log is the
     * point: if a render service is misconfigured to emit one of these, the
     * next step is obvious instead of being a mystery byte dump.
     */
    if (data[0] == 0xFF && data[1] == 0xD8) {
        ESP_LOGE(TAG, "received JPEG; this firmware decodes PNG (or 1bpp BMP) only "
                      "(the P4 has a hardware JPEG decoder if this becomes worth adding)");
    } else if (memcmp(data, "BB", 2) == 0) {
        ESP_LOGE(TAG, "received a G5-compressed bitmap; this firmware decodes PNG (or 1bpp BMP) only");
    } else {
        ESP_LOGE(TAG, "unrecognised image format: %02X %02X %02X %02X",
                 data[0], data[1], data[2], data[3]);
    }
    return ESP_ERR_NOT_SUPPORTED;
}
