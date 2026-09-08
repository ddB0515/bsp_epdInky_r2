#include "epd_display.h"
#include "epd_i80_bus.h"
#include "tps65185.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "epd_display";

/*******************************************************************************
 * Panel-specific constants have MOVED
 *
 * AC timings, the waveform/tone model and the source drive codes are no longer
 * compile-time constants here.  They belong to the panel, so they live in an
 * epd_panel_def_t (see epd_panel_def.h) and the concrete values are in
 * epd_panels.c.  The driver reads them at runtime from priv->def, which is what
 * lets one build support several panels.
 *
 * Notes that used to sit on those constants, kept here because they are about
 * the driver's behaviour rather than about any one panel:
 *
 * ROW PERIOD (timing.row_period_us)
 *   The LE-to-LE interval: how long one row's data is held on the panel
 *   outputs.  0 means free-run, where the period is whatever the row build, the
 *   DMA and the scheduler happen to add up to; a non-zero value paces every row
 *   to exactly that figure, so drive dwell becomes a tuning constant rather
 *   than a by-product of compiler flags and cache behaviour.
 *
 *   With EPD_ROW_STATS enabled each refresh logs its own period:
 *
 *       71 us/row = build 36 + wait 5 + submit 29 | dma 13, wake 5
 *
 *   build + wait + submit is the period; pick a value at or above it.
 *   "overruns" counts rows that could not meet it, so a non-zero count means
 *   the value is too low.  The floor is the row DMA itself (width/4 + 16 bytes
 *   at pclk_hz) and the ceiling is CKV's minimum period.
 *
 *   Note that the reference panel proved insensitive to this: dwell was cut
 *   about 30 % over two optimisation steps with no visible change, because its
 *   tone comes from how many phases drive a pixel plus the Bayer offset, not
 *   from how long each row is held.
 *
 * POWER-DOWN TIMINGS (timing.*_ms, timing.discharge_frames)
 *   vcom_off_settle_ms must be long enough for the VCOM node - a large
 *   capacitance - to actually reach ground, not merely for the register write
 *   to land, because the neutralising scan is only bias-free once it has.
 *
 *   The per-row neutralising scan only connects each pixel for the ~15 us its
 *   row is addressed, far too short to bleed a pixel capacitor through the
 *   TFT's on-resistance.  global_discharge_ms opens every row at once and holds
 *   them, so all pixels drain in parallel for as long as needed.
 *
 *   Set discharge_frames to 0 to skip the neutralising scan entirely if the
 *   image is still disturbed at shutdown.
 ******************************************************************************/

/* Log per-refresh row timing statistics (see the row period notes above). */
#define EPD_ROW_STATS      1

/*
 * How long to wait for the PMIC rails after power-up, and how often to look.
 *
 * The generous timeout costs nothing when the rails are healthy - the loop
 * exits as soon as they are - and only applies when something is genuinely
 * wrong.  Measured worst case on the epdInky board is ~300 ms for VDDH.
 */
#define EPD_PG_TIMEOUT_MS  800
#define EPD_PG_POLL_MS      20

/*******************************************************************************
 * Waveform pixel encoding — 2 bits per pixel, 4 pixels packed per byte
 *
 * Byte layout (source bus):
 *   bits[7:6] = pixel 0 (leftmost in group)
 *   bits[5:4] = pixel 1
 *   bits[3:2] = pixel 2
 *   bits[1:0] = pixel 3 (rightmost in group)
 *
 * WHICH code drives which way is a property of the panel's source driver IC,
 * so the values live in epd_panel_def_t::codes (see epd_panels.c) rather than
 * being hardcoded here.  On the reference panel they are hardware-confirmed as:
 *
 *   no_drive 0b00   leaves the pixel undriven
 *   darken   0b01   VNEG, drives toward black
 *   lighten  0b10   VPOS, drives toward white
 *
 * "no drive" is NOT the same as "no field".  It leaves the source line near
 * ground while VCOM is live at its operating voltage, so an undriven pixel
 * still sees a DC bias of the full VCOM magnitude for the one row period per
 * frame that its gate is open.  That is bounded during a normal refresh, where
 * a pixel is undriven for only some frames, but it is what makes windowed
 * partial updates bleed on this panel - the masked region never gets driven
 * back.  The power-off sequence at the bottom of this file deals with the same
 * effect by pulling VCOM to 0 V before its neutralising scan.
 *
 * The fourth code, 0b11, is undocumented for the reference panel.  It was
 * tested on the hypothesis that it might be a high-impedance state usable for
 * masking; it is not.
 ******************************************************************************/

/* Pack one 2-bit waveform value into all four pixel slots of a byte. */
static inline uint8_t wf_uniform(uint8_t wf)
{
    return (uint8_t)((wf << 6) | (wf << 4) | (wf << 2) | wf);
}

/*******************************************************************************
 * Private state
 ******************************************************************************/
/*
 * Row period accounting.  These four buckets sum to the measured row period:
 *
 *   build  — caller building the row (now overlapped with the previous DMA)
 *   wait   — blocked in wait_row: residual DMA plus scheduler wake latency
 *   submit — gate_advance() + queuing the next DMA (esp_lcd tx_color)
 *
 * dma/wake further split what a NON-overlapped wait would have cost, using the
 * ISR completion timestamp, so the pre-pipeline row period can be reported
 * exactly rather than estimated.
 */
typedef struct {
    int64_t  busy_us;    /* wall time spent inside frame row loops          */
    int64_t  build_us;   /* of which: CPU building rows                     */
    int64_t  wait_us;    /* of which: blocked in wait_row                   */
    int64_t  submit_us;  /* of which: gate edges + queuing the DMA          */
    int64_t  dma_us;     /* submit-return -> completion ISR (hardware)      */
    int64_t  wake_us;    /* completion ISR -> waiting task resumed          */
    uint32_t frames;     /* frames sent                                     */
    uint32_t rows;       /* rows sent                                       */
    uint32_t overruns;   /* rows that missed T_ROW_PERIOD_US                */
} epd_row_stats_t;

typedef struct {
    /**
     * Panel description: geometry, AC timing and waveform model.
     *
     * Points at def_storage below, never at the caller's struct. The driver
     * reads it on every row of every refresh, so borrowing the caller's
     * pointer would mean a definition on their stack turns to garbage the
     * moment that frame is gone - which shows up as a refresh that wanders
     * off mid-frame, long after the call that caused it.
     */
    const epd_panel_def_t *def;
    epd_panel_def_t        def_storage;

    /*
     * Two DMA row buffers, used alternately: the CPU builds row N+1 into one
     * while the DMA engine is still reading row N out of the other.
     */
    uint8_t *row_buf[2];
    size_t   row_alloc;  /* bytes allocated per row buffer                  */
    size_t   row_bytes;  /* = width / 4  (2bpp -> 4 px per byte)            */
    uint8_t  flags;      /* EPD_PANEL_FLAG_* mirror bits                    */
    bool     temp_comp;  /* scale frame counts by panel temperature         */
    int8_t   temp_c;     /* last temperature read from the PMIC thermistor  */
    uint16_t frame_pct;  /* frame-count scale in percent (100 = nominal)    */
    epd_row_stats_t stats;

    /*
     * Per-instance lookup tables.  These were file-scope statics, which meant
     * two panels driven from one application would silently corrupt each
     * other's tables; with panel definitions selectable at runtime that is a
     * real possibility rather than a theoretical one.
     */
    /*
     * [y & 7][output byte & 1][half][source byte].
     *
     * The Bayer period is 8 pixels but an output byte carries only 4, so an
     * output byte spans half a period and its parity selects which half.  With
     * a 4x4 matrix both parities held the same values and the index was not
     * needed.
     */
    uint8_t  gc16_lut[8][2][2][256];
    int      gc16_lut_phase;
    bool     gc16_lut_mirror;
    uint8_t  du_diff_lut[2][256];   /* [parity][(prev_nibble<<4)|next_nibble] */
    int      du_diff_lut_threshold; /* threshold built for, -1 = none      */
} epd_display_priv_t;

/** Bytes actually clocked out per row: active data + 16 bytes line padding. */
/** Bytes clocked out per row: active data plus the panel's line padding. */
#define ROW_TX_BYTES(priv)  ((priv)->row_bytes + (priv)->def->line_padding_bytes)

/*******************************************************************************
 * Temperature compensation
 *
 * Particle mobility in the microcapsules is strongly temperature dependent: a
 * frame count tuned at ~25 C leaves the image washed out in the cold and
 * over-driven (with visible ghost-burn) in the heat.  Frame counts are scaled
 * by a percentage derived from the PMIC thermistor.
 *
 * The nominal band returns exactly 100 %, so at room temperature the driver
 * reproduces the empirically tuned behaviour bit-for-bit.
 ******************************************************************************/

/*
 * Plausibility window for a thermistor reading, in Celsius.
 *
 * tps65185_read_temperature() casts the raw TMST_VALUE register straight to
 * int8_t without checking that a conversion actually completed, so a missing,
 * open or shorted thermistor can return a railed value such as -128.  Feeding
 * that to temp_frame_pct() would silently more than double the drive energy of
 * every waveform, so out-of-range readings are rejected and the nominal tuning
 * is used instead.
 */
#define TEMP_VALID_MIN_C  (-25)
#define TEMP_VALID_MAX_C   (85)

static uint16_t temp_frame_pct(int8_t t)
{
    if (t <   0) return 220;
    if (t <  10) return 170;
    if (t <  15) return 135;
    if (t <  30) return 100;   /* nominal tuning band */
    if (t <  40) return 85;
    return 75;
}

/* Scale a nominal frame count, never returning less than one frame. */
static inline int scale_frames(const epd_display_priv_t *priv, int nominal)
{
    int n = (nominal * (int)priv->frame_pct + 50) / 100;
    return (n < 1) ? 1 : n;
}

/*******************************************************************************
 * Gate timing helpers
 *
 * Gate driver samples SPV on the FALLING edge of CKV
 * (tSU/tH setup+hold times reference CKV↓, max = twH-100 means SPV must be
 * asserted during the HIGH period).
 *
 * Frame start:
 *   CKV↑  →  SPV=0 (gate start)  →  delay≥tSU  →  CKV↓ (latch row-0 start)
 *   →  delay≥tH  →  SPV=1
 *
 * Per row, as implemented by gate_advance() - note CKV is HIGH across the DMA:
 *   CKV↓ (activates row y-1) → LE pulse (latches row y-1's data, shifted in
 *   during the previous iteration) → ckv_low_us → CKV↑ → SPH↓ → DMA of row y
 *   (CL clocks data) → SPH↑
 *
 * So the loop runs one row ahead of the panel: row y's data is shifted in
 * while row y-1 is the active gate line being driven.  See EINK_DETAILS.md.
 ******************************************************************************/

/*
 * Drive CKV low for at least the panel's minimum low pulse width, then high.
 *
 * Without a stretch the two GPIO writes give a low pulse of roughly 100 ns,
 * which is below the twL some panels specify.  timing.ckv_low_us == 0 keeps the
 * original back-to-back behaviour exactly.
 */
/*
 * Pulse LE high for at least the panel's minimum pulse width, then low.
 *
 * Two GPIO writes alone give only tens of nanoseconds, which is below the tLEw
 * some panels specify.  timing.le_pulse_us == 0 keeps the original behaviour.
 */
static inline void le_pulse(epd_i80_bus_handle_t bus, const epd_ac_timing_t *t)
{
    epd_i80_le_high(bus);
    if (t->le_pulse_us) {
        esp_rom_delay_us(t->le_pulse_us);
    }
    epd_i80_le_low(bus);
}

static inline void ckv_pulse(epd_i80_bus_handle_t bus, const epd_ac_timing_t *t)
{
    epd_i80_ckv_low(bus);
    if (t->ckv_low_us) {
        esp_rom_delay_us(t->ckv_low_us);
    }
    epd_i80_ckv_high(bus);
}

static void frame_gate_start(epd_i80_bus_handle_t bus,
                              const epd_ac_timing_t *t)
{
    /*
     * OE stays enabled throughout — do not toggle it here.
     *
     * Sequence:
     *   CKV↑ — delay — SPV=0 — delay — CKV↓ — CKV↑ — delay — SPV=1
     *   — delay — CKV↓ — CKV↑ — delay — CKV↓ — CKV↑ — delay — CKV↓
     *   — CKV↑   (ends with CKV HIGH, gate positioned at row 0)
     *
     * Run atomically: an ISR landing between two edges here would shift the
     * gate start and offset the whole frame by a row.  Total time is ~55 us,
     * which is acceptable to hold interrupts off for once per frame.
     */
    epd_i80_bus_enter_critical(bus);

    epd_i80_ckv_high(bus);
    esp_rom_delay_us(t->ckv_pre_spv_us);
    epd_i80_spv_low(bus);
    esp_rom_delay_us(t->spv_low_us);

    /*
     * Clock the frame sync.  A panel asking for t1 >= 2 lines needs SPV held
     * across that many CKV pulses; with spv_sync_lines == 1 this is exactly the
     * single pulse the sequence has always emitted.
     */
    for (uint8_t i = 0; i < t->spv_sync_lines; i++) {
        ckv_pulse(bus, t);
        if (i + 1u < t->spv_sync_lines) {
            esp_rom_delay_us(t->ckv_extra_us);
        }
    }

    esp_rom_delay_us(t->ckv_post_spv_us);
    epd_i80_spv_high(bus);
    esp_rom_delay_us(t->spv_high_us);
    ckv_pulse(bus, t);
    esp_rom_delay_us(t->ckv_extra_us);
    ckv_pulse(bus, t);
    esp_rom_delay_us(t->ckv_extra_us);
    ckv_pulse(bus, t);       /* CKV ends HIGH - "row 0" on every panel validated before ED067KC1 */

    /*
     * Extra blank lines some panels need beyond that fixed three-pulse walk -
     * see the field comment on frame_blank_lines. 0 for every panel that
     * does not set it, so this loop does not run and behaviour is identical
     * to before the field existed.
     */
    for (uint16_t i = 0; i < t->frame_blank_lines; i++) {
        esp_rom_delay_us(t->ckv_extra_us);
        ckv_pulse(bus, t);
    }

    epd_i80_bus_exit_critical(bus);
}

/* Restore idle bus state after the last row has been shifted. */
static void frame_gate_end(epd_i80_bus_handle_t bus, const epd_ac_timing_t *t)
{
    epd_i80_bus_enter_critical(bus);

    /* Latch the last row's source data before advancing past it */
    epd_i80_ckv_low(bus);
    le_pulse(bus, t);       /* commit the last row's shift-register data */
    epd_i80_spv_high(bus);
    /* SPH is hardware CS — idle HIGH is maintained by the LCD peripheral */

    epd_i80_bus_exit_critical(bus);
}

/*
 * Advance the gate driver by one row and latch the previously shifted row's
 * source data onto the panel outputs.
 *
 *   1. CKV↓  — advances the gate to the next row
 *   2. LE↑↓  — latches the PREVIOUS row's source data to the panel outputs
 *   3. CKV↑  — re-arms the gate clock
 *
 * For the first row of a frame there is no previous row to latch, so steps
 * 1 and 2 are skipped and only CKV↑ is asserted.
 *
 * The caller MUST have waited for the previous row's DMA before calling this:
 * the LE pulse commits whatever is sitting in the source shift register, so
 * pulsing it while a transfer is still in flight would latch a half-shifted
 * row.
 */
static inline void gate_advance(epd_i80_bus_handle_t bus,
                                 const epd_ac_timing_t *t, bool first_row)
{
    epd_i80_bus_enter_critical(bus);
    if (!first_row) {
        epd_i80_ckv_low(bus);    /* advance gate to this row               */
        le_pulse(bus, t);        /* latch previous row data to outputs      */
        /*
         * The LE pulse alone holds CKV low for only a couple of GPIO writes.
         * Stretch it when the panel specifies a minimum low width; this is per
         * row, so it adds directly to the row period.
         */
        if (t->ckv_low_us) {
            esp_rom_delay_us(t->ckv_low_us);
        }
    }
    epd_i80_ckv_high(bus);       /* CKV HIGH before DMA  */
    epd_i80_bus_exit_critical(bus);
}

/*******************************************************************************
 * Row pipeline
 *
 * Overlaps the CPU build of row N+1 with the DMA of row N, and paces the
 * LE→LE row period to T_ROW_PERIOD_US so the drive dwell is a tuning constant
 * rather than a by-product of build time and scheduler latency.
 *
 * Usage:
 *
 *     epd_row_pipe_t p;
 *     row_pipe_begin(&p, bus, priv);          // emits frame_gate_start()
 *     for (y = 0; y < HEIGHT; y++) {
 *         build_row(row_pipe_buf(&p), y);     // overlaps row y-1's DMA
 *         ret = row_pipe_submit(&p);
 *         if (ret != ESP_OK) return row_pipe_abort(&p, ret);
 *     }
 *     return row_pipe_finish(&p);
 *
 * Note the resulting ordering per iteration: build(y) → wait(DMA y-1) →
 * gate_advance() → submit(y).  The gate still advances only once row y-1 has
 * fully shifted in, exactly as before; the only change is that the build now
 * happens during that transfer instead of after it.
 ******************************************************************************/
typedef struct {
    epd_i80_bus_handle_t bus;
    epd_display_priv_t  *priv;
    int      cur;           /* index of the buffer being built into      */
    bool     first;         /* nothing in flight yet                     */
    int64_t  deadline_us;   /* when the next gate_advance() may happen    */
    int64_t  start_us;      /* frame loop entry, for statistics          */
    int64_t  submitted_us;  /* end of the last submit, for build timing  */
} epd_row_pipe_t;

/* Hold off until this row's slot in the T_ROW_PERIOD_US grid comes up. */
static inline void row_pipe_pace(epd_row_pipe_t *p)
{
    const uint16_t period = p->priv->def->timing.row_period_us;
    if (period == 0) {
        return;
    }

    p->deadline_us += period;

    int64_t now = esp_timer_get_time();
    if (now < p->deadline_us) {
        while (esp_timer_get_time() < p->deadline_us) {
            /* busy-wait: a vTaskDelay here would hand the CPU to another task
             * and blow straight through the deadline we are trying to hold */
        }
        return;
    }

    p->priv->stats.overruns++;
    /*
     * The row took longer than its slot.  Re-synchronise rather than trying to
     * claw the time back on later rows, which would make several rows in a row
     * run with no dwell at all.
     */
    if (now - p->deadline_us > period) {
        p->deadline_us = now;
    }
}

static void row_pipe_begin(epd_row_pipe_t     *p,
                            epd_i80_bus_handle_t bus,
                            epd_display_priv_t  *priv)
{
    p->bus   = bus;
    p->priv  = priv;
    p->cur   = 0;
    p->first = true;

    frame_gate_start(bus, &priv->def->timing);   /* ends with CKV HIGH */

    p->start_us     = esp_timer_get_time();
    p->deadline_us  = p->start_us;
    p->submitted_us = p->start_us;
}

/** Buffer the caller should build the next row into. */
static inline uint8_t *row_pipe_buf(const epd_row_pipe_t *p)
{
    return p->priv->row_buf[p->cur];
}

/*
 * Commit the row currently in row_pipe_buf(): wait out the in-flight transfer,
 * hold the row period, advance the gate, and queue this row's DMA.
 */
static esp_err_t row_pipe_submit(epd_row_pipe_t *p)
{
#if EPD_ROW_STATS
    /* Everything since the last submit returned was the caller building this
     * row — the work that now overlaps the previous row's DMA. */
    const int64_t t_built = esp_timer_get_time();
    p->priv->stats.build_us += t_built - p->submitted_us;
#endif

    /* Row y-1's data has to be fully shifted in before LE commits it. */
    esp_err_t ret = epd_i80_bus_wait_row(p->bus);

#if EPD_ROW_STATS
    const int64_t t_waited = esp_timer_get_time();
    p->priv->stats.wait_us += t_waited - t_built;
    if (!p->first) {
        /*
         * dma  — how long the transfer itself took, from the submit returning
         *        until the completion ISR fired.  This may be entirely hidden
         *        behind the row build, so it does NOT have to fit inside
         *        wait_us.
         * wake — scheduler latency only: measured from whichever happened
         *        LAST, the ISR or the build finishing.  Measuring from the ISR
         *        unconditionally would count the tail of the build as wake
         *        time whenever the DMA finished first.
         */
        const int64_t done = epd_i80_bus_last_done_us(p->bus);
        if (done > p->submitted_us && done <= t_waited) {
            p->priv->stats.dma_us  += done - p->submitted_us;
            p->priv->stats.wake_us += t_waited - ((done > t_built) ? done : t_built);
        }
    }
#endif

    if (ret != ESP_OK) {
        return ret;
    }

    if (p->first) {
        /* No previous row is being driven yet, so there is no dwell to hold —
         * just start the pacing grid here. */
        p->deadline_us = esp_timer_get_time();
    } else {
        row_pipe_pace(p);
    }

    gate_advance(p->bus, &p->priv->def->timing, p->first);

    ret = epd_i80_bus_send_row_async(p->bus, row_pipe_buf(p),
                                     ROW_TX_BYTES(p->priv));
    if (ret != ESP_OK) {
        return ret;
    }

    p->first = false;
    p->cur  ^= 1;
    p->priv->stats.rows++;
#if EPD_ROW_STATS
    p->submitted_us = esp_timer_get_time();
    p->priv->stats.submit_us += p->submitted_us - t_waited;
#endif
    return ESP_OK;
}

/* Drain the last row, settle, and restore idle bus state. */
static esp_err_t row_pipe_finish(epd_row_pipe_t *p)
{
#if EPD_ROW_STATS
    const int64_t t_enter = esp_timer_get_time();
#endif

    esp_err_t ret = epd_i80_bus_wait_row(p->bus);

#if EPD_ROW_STATS
    p->priv->stats.wait_us += esp_timer_get_time() - t_enter;
#endif

    /* Give the final row the same dwell every other row got. */
    row_pipe_pace(p);

    p->priv->stats.busy_us += esp_timer_get_time() - p->start_us;
    p->priv->stats.frames++;

    esp_rom_delay_us(p->priv->def->timing.interframe_us);  /* let last DMA/gate settle */
    frame_gate_end(p->bus, &p->priv->def->timing);
    return ret;
}

/* Error exit: never leave a DMA in flight or the bus mutex held. */
static esp_err_t row_pipe_abort(epd_row_pipe_t *p, esp_err_t err)
{
    epd_i80_bus_wait_row(p->bus);
    frame_gate_end(p->bus, &p->priv->def->timing);
    return err;
}

/*******************************************************************************
 * Update window
 *
 * A partial update still has to clock every gate row — the gate driver is a
 * shift register with no random access — so "partial" means we send no-drive
 * data outside the region rather than skipping rows entirely.  Pixels that
 * receive no drive keep their charge and are left visually untouched.
 *
 * Rows are tracked in framebuffer coordinates.  Columns are converted to
 * OUTPUT byte indices up front, because that is the granularity at which a row
 * can be masked (4 pixels per transferred byte).
 ******************************************************************************/
/** Transferred bytes per row: 2bpp packing puts 4 pixels in a byte. */
static inline int row_out_bytes_of(const epd_panel_def_t *def)
{
    return def->width / 4;
}

typedef struct {
    uint16_t row_lo, row_hi;   /* framebuffer row range   [lo, hi) */
    int      col_lo, col_hi;   /* output byte-index range [lo, hi) */
} epd_window_t;

static void window_full(const epd_panel_def_t *def, epd_window_t *w)
{
    w->row_lo = 0;
    w->row_hi = def->height;
    w->col_lo = 0;
    w->col_hi = row_out_bytes_of(def);
}

/*
 * Convert a framebuffer rectangle into an output window.
 *
 * Horizontal bounds are snapped outward to whole output bytes, so the driven
 * area is always a superset of the request.  Under MIRROR_X the framebuffer
 * range maps to the opposite end of the display row.
 */
static void window_from_rect(const epd_panel_def_t *def, const epd_rect_t *r,
                              bool mirror_x, epd_window_t *w)
{
    w->row_lo = r->y;
    w->row_hi = (uint16_t)(r->y + r->h);

    const int fb_lo = (int)r->x;
    const int fb_hi = (int)r->x + (int)r->w;
    const int dsp_lo = mirror_x ? (def->width - fb_hi) : fb_lo;
    const int dsp_hi = mirror_x ? (def->width - fb_lo) : fb_hi;

    w->col_lo = dsp_lo / 4;              /* round down */
    w->col_hi = (dsp_hi + 3) / 4;        /* round up   */
}

static inline bool window_is_full(const epd_panel_def_t *def,
                                   const epd_window_t *w)
{
    return w->row_lo == 0 && w->row_hi == def->height &&
           w->col_lo == 0 && w->col_hi == row_out_bytes_of(def);
}

/*
 * Zero the output bytes that fall outside the horizontal update window so the
 * corresponding pixels receive no drive.
 *
 * col_lo / col_hi are OUTPUT byte indices (already converted from framebuffer
 * coordinates and mirror-adjusted by window_from_rect()).
 */
/*
 * A source byte carries four pixels, so a uniform code is replicated into all
 * four 2-bit fields: 0b11 -> 0xFF, 0b00 -> 0x00.
 */
static inline uint8_t code_fill_byte(uint8_t code)
{
    return (uint8_t)(code * 0x55u);
}

static void mask_row_columns(uint8_t *row_buf, int row_out_bytes,
                              int col_lo, int col_hi, uint8_t fill)
{
    if (col_lo > 0) {
        memset(row_buf, fill, (size_t)col_lo);
    }
    if (col_hi < row_out_bytes) {
        memset(row_buf + col_hi, fill, (size_t)(row_out_bytes - col_hi));
    }
}

/*******************************************************************************
 * Frame senders
 ******************************************************************************/

/* Send one full frame with the same byte value in every row (INIT/discharge). */
static esp_err_t send_frame_uniform(epd_panel_handle_t  panel,
                                     epd_display_priv_t *priv,
                                     uint8_t             fill)
{
    epd_i80_bus_handle_t bus = epd_panel_get_bus(panel);
    const epd_panel_def_t *def = priv->def;

    /*
     * Fill the active pixels of BOTH ping-pong buffers; padding bytes (beyond
     * row_bytes) stay zero.  The buffer never changes after that, so this path
     * does no per-row CPU work at all.
     */
    memset(priv->row_buf[0], fill, priv->row_bytes);
    memset(priv->row_buf[1], fill, priv->row_bytes);

    epd_row_pipe_t p;
    row_pipe_begin(&p, bus, priv);

    for (uint16_t y = 0; y < def->height; y++) {
        esp_err_t ret = row_pipe_submit(&p);
        if (ret != ESP_OK) return row_pipe_abort(&p, ret);
    }

    return row_pipe_finish(&p);
}

/*******************************************************************************
 * GC16 tone model
 *
 * The panel resolves far fewer than 16 distinct tones on its own, so grey is
 * synthesised by combining a small number of temporal phases with a 4x4 spatial
 * dither.  A pixel drives in phase p while
 *
 *     level_energy[level] + bayer[y & 7][x & 7]  >=  phase_cut[phase_order[p]]
 *
 * so the number of driving frames rises with energy, and the Bayer offset
 * dithers the threshold to fill in between phases.
 *
 * The tables themselves are per-panel and live in epd_panel_def_t::wf (see
 * epd_panels.c).  Tuning knobs, in the order worth reaching for:
 *
 *   gc16_level_energy[16]  darkness energy 0..64 per grey level; the main tone
 *                          curve.  0 = black .. 15 = white, higher energy means
 *                          more darkening pulses.  Nonlinear spacing expected.
 *   gc16_phase_cut[]       per-phase thresholds; lower means more pixels drive
 *                          in that phase, so a darker average.
 *   gc16_phase_order[]     reorders passes, for panels whose response differs
 *                          between early and late phases.
 *   bayer[8][8]            spatial distribution of drive events; reduces
 *                          contouring by spreading transitions across a
 *                          neighbourhood.
 *
 * Calibrate against the bars and palette screens in main.c: photograph under
 * constant lighting, find which levels have collapsed together, adjust
 * level_energy around them, then adjust phase_cut globally.
 *
 * Note that this model is ABSOLUTE - it assumes the panel starts from the white
 * baseline waveform_init leaves behind, and each phase can only darken.  That
 * assumption is load-bearing.
 ******************************************************************************/

/*
 * GC16 row-build lookup table.
 *
 * The drive code for a pixel depends only on (grey level, phase, src_x & 3,
 * src_y & 3).  Phase is constant for a whole frame, so everything except the
 * source byte can be folded into a table that is rebuilt once per frame.
 *
 * Layout: priv->gc16_lut[y & 3][half][source_byte] -> 4-bit packed pixel pair
 *   bits [3:2] = first output pixel of the pair
 *   bits [1:0] = second output pixel of the pair
 *
 * One 4bpp source byte carries exactly two pixels, and one 2bpp output byte
 * carries four, so each output byte consumes two source bytes: "half 0" is the
 * left/leading source byte and "half 1" the right/trailing one.
 *
 * This replaces the previous per-pixel inner loop (WIDTH iterations per row,
 * ~10.5 M iterations per GC16 refresh) with width/4 table-driven iterations.
 *
 * The table lives in the panel instance rather than at file scope, so two
 * panels driven from one application cannot corrupt each other's.
 */
static inline uint8_t gc16_code(const epd_waveform_def_t *wf,
                                 const epd_drive_codes_t  *codes,
                                 uint8_t level, uint16_t cut, int xb, int yb)
{
    /* uint16_t: energy and Bayer can each reach 255 and 63 respectively. */
    uint16_t score = (uint16_t)(wf->gc16_level_energy[level & 0x0Fu] +
                                wf->bayer[yb][xb]);
    return (score >= cut) ? codes->darken : codes->no_drive;
}

static void gc16_build_lut(epd_display_priv_t *priv, int phase, bool mirror_x)
{
    if (priv->gc16_lut_phase == phase && priv->gc16_lut_mirror == mirror_x) return;

    const epd_waveform_def_t *wf    = &priv->def->wf;
    const epd_drive_codes_t  *codes = &priv->def->codes;
    const uint16_t cut = wf->gc16_phase_cut[wf->gc16_phase_order[phase]];

    for (int yb = 0; yb < 8; yb++) {
        for (int par = 0; par < 2; par++) {
            for (int half = 0; half < 2; half++) {
                /*
                 * Source-x of the two pixels this half emits, modulo the 8-wide
                 * Bayer period.  The dither is applied in SOURCE coordinates,
                 * so a mirrored row walks the period backwards: width is a
                 * multiple of 8, so source x for the first pixel of output byte
                 * b is congruent to 7 - (4*(b&1)) mod 8.
                 */
                const int base = par * 4 + half * 2;
                const int xb0 = mirror_x ? (7 - base) : base;
                const int xb1 = mirror_x ? (6 - base) : (base + 1);

                for (int b = 0; b < 256; b++) {
                    uint8_t hi = (uint8_t)(b >> 4);
                    uint8_t lo = (uint8_t)(b & 0x0Fu);
                    uint8_t c0 = gc16_code(wf, codes, mirror_x ? lo : hi, cut, xb0, yb);
                    uint8_t c1 = gc16_code(wf, codes, mirror_x ? hi : lo, cut, xb1, yb);
                    priv->gc16_lut[yb][par][half][b] = (uint8_t)((c0 << 2) | c1);
                }
            }
        }
    }

    priv->gc16_lut_phase  = phase;
    priv->gc16_lut_mirror = mirror_x;
}

/* Build one GC16 row from the next 4bpp row using the pre-built phase LUT. */
static void build_row_gc16(const epd_display_priv_t *priv,
                            uint8_t       *row_buf,
                            const uint8_t *next_row,
                            bool           mirror_x,
                            int            row_y)
{
    const uint8_t (*lut)[2][256] = priv->gc16_lut[row_y & 7];
    const uint16_t width = priv->def->width;
    const int row_out_bytes = width / 4;   /* 2bpp -> 4 px per output byte */

    /*
     * Output bytes alternate between the two halves of the 8-pixel Bayer
     * period, so they are emitted in pairs and the parity index is resolved at
     * compile time inside the loop body rather than per byte.  width is a
     * multiple of 8, so row_out_bytes is always even and the pairing is exact.
     */
    if (mirror_x) {
        const uint8_t *src = next_row + (width / 2) - 1;   /* last source byte */
        for (int b = 0; b < row_out_bytes; b += 2) {
            row_buf[b]     = (uint8_t)((lut[0][0][src[0]]  << 4) | lut[0][1][src[-1]]);
            row_buf[b + 1] = (uint8_t)((lut[1][0][src[-2]] << 4) | lut[1][1][src[-3]]);
            src -= 4;
        }
    } else {
        const uint8_t *src = next_row;
        for (int b = 0; b < row_out_bytes; b += 2) {
            row_buf[b]     = (uint8_t)((lut[0][0][src[0]] << 4) | lut[0][1][src[1]]);
            row_buf[b + 1] = (uint8_t)((lut[1][0][src[2]] << 4) | lut[1][1][src[3]]);
            src += 4;
        }
    }
}

/* Send one GC16 frame for a given transition phase. */
static esp_err_t send_frame_gc16(epd_panel_handle_t  panel,
                                  epd_display_priv_t *priv,
                                  const uint8_t      *next_buf,
                                  int                 phase,
                                  const epd_window_t *win)
{
    epd_i80_bus_handle_t bus   = epd_panel_get_bus(panel);
    const epd_panel_def_t *def = priv->def;
    const size_t row_src_bytes = def->width / 2;   /* 4bpp: 2 pixels per byte */
    const bool mirror_y = (priv->flags & EPD_PANEL_FLAG_MIRROR_Y) != 0;
    const bool mirror_x = (priv->flags & EPD_PANEL_FLAG_MIRROR_X) != 0;
    const bool partial  = !window_is_full(def, win);

    /*
     * Fold phase / mirror / bayer into a table once, before touching the panel,
     * so no LUT work happens between gate edges.
     */
    gc16_build_lut(priv, phase, mirror_x);

    epd_row_pipe_t p;
    row_pipe_begin(&p, bus, priv);

    for (uint16_t y = 0; y < def->height; y++) {
        const uint16_t src_y = mirror_y ? (def->height - 1u - y) : y;
        const uint8_t *next_row = next_buf + (size_t)src_y * row_src_bytes;
        uint8_t *row_buf = row_pipe_buf(&p);

        /*
         * Build every row, including rows outside the update window, then zero
         * the ones that fall outside.  Skipping the build for out-of-window
         * rows would make the per-row CPU time vary at the window edges; with
         * T_ROW_PERIOD_US pacing that no longer changes the drive dwell, but
         * it would still cost a spurious overrun report, and it keeps the free-
         * running (T_ROW_PERIOD_US == 0) path uniform.
         */
        build_row_gc16(priv, row_buf, next_row, mirror_x, (int)src_y);

        if (src_y < win->row_lo || src_y >= win->row_hi) {
            memset(row_buf, 0, priv->row_bytes);
        } else if (partial) {
            mask_row_columns(row_buf, row_out_bytes_of(def),
                             win->col_lo, win->col_hi,
                             code_fill_byte(priv->def->codes.no_drive));
        }

        esp_err_t ret = row_pipe_submit(&p);
        if (ret != ESP_OK) return row_pipe_abort(&p, ret);
    }
    return row_pipe_finish(&p);
}

/*
 * Differential DU lookup: 256 entries indexed by (prev_nibble<<4)|next_nibble,
 * a hard 2-level decision instead of a magnitude/cut threshold - DU has no
 * grey phases to dither.
 *
 * Earlier this only looked at the target level: below threshold -> darken,
 * above -> idle. That is a strict "add black ink" model with no way to ever
 * drive a pixel back to white, so a pixel already darkened by a previous DU
 * pass stays darkened forever - exactly the "old digit ghosts through the
 * new one" failure. Bringing in prev lets a pixel that is currently black but
 * should now be white get an active lighten pass instead of being left alone.
 *
 * Passing prev_buf == NULL (see send_frame_du()) treats every pixel as
 * starting from white, which reduces exactly to the old threshold-only
 * behaviour.
 *
 * This is still a lookup table, not the "112 us/row" per-pixel branching
 * version an earlier comment here warned about - build_row_du() costs four
 * lookups per output byte and runs at 33 us/row.
 */
static void du_build_diff_lut(epd_display_priv_t *priv, uint8_t threshold)
{
    if (priv->du_diff_lut_threshold == (int)threshold) {
        return;
    }

    const epd_drive_codes_t *codes = &priv->def->codes;

    /*
     * Neither "not driving" code is actually neutral, and they are not neutral
     * in the same direction.  Measured on ED103TC2 with a step wedge of no-op
     * DU passes (0, 1, 2, 5, 10, 20) over identical white:
     *
     *   no_drive (0b00)   source grounded   -> the area drifts DARKER
     *   hold     (0b11)   source released   -> the area drifts LIGHTER
     *
     * Both are dose-dependent and invisible for one or two passes, which is why
     * this only shows up once several updates have accumulated.
     *
     * Since the two bias opposite ways, alternating them frame by frame cancels
     * the charge instead of accumulating it - the same DC-balancing principle
     * the drive waveforms themselves use.  du_frames is even, so a complete DU
     * update ends balanced.  Only applies to pixels that are staying white -
     * a pixel actively being darkened or lightened does not need it.
     */
    const uint8_t light_code[2] = { codes->no_drive, codes->hold };

    for (int par = 0; par < 2; par++) {
        for (int idx = 0; idx < 256; idx++) {
            const uint8_t prev = (uint8_t)(idx >> 4);
            const uint8_t next = (uint8_t)(idx & 0x0Fu);

            uint8_t code;
            if (next < threshold) {
                code = codes->darken;              /* target black                */
            } else if (prev < threshold) {
                code = codes->lighten;             /* was black, now target white */
            } else {
                code = light_code[par];            /* stays white - idle          */
            }
            priv->du_diff_lut[par][idx] = code;
        }
    }
    priv->du_diff_lut_threshold = (int)threshold;
}

/*
 * Build one differential DU row: since DU has no grey phases to dither, one
 * flat 256-entry table serves the whole row.  prev_row may be NULL (see
 * send_frame_du()), in which case every prev nibble reads as white (0xF).
 */
static void build_row_du(const epd_display_priv_t *priv,
                          uint8_t       *row_buf,
                          const uint8_t *prev_row,
                          const uint8_t *next_row,
                          bool           mirror_x,
                          int            parity)
{
    const uint16_t width = priv->def->width;
    const int row_out_bytes = width / 4;   /* 2bpp -> 4 px per output byte */
    const uint8_t *lut = priv->du_diff_lut[parity];

    if (mirror_x) {
        /* Mirrored: walk backwards, so each byte yields its LOW nibble first. */
        const uint8_t *n = next_row + (width / 2) - 1;   /* last source byte */
        const uint8_t *p = prev_row ? prev_row + (width / 2) - 1 : NULL;
        for (int b = 0; b < row_out_bytes; b++) {
            const uint8_t n0 = n[0], n1 = n[-1];
            const uint8_t p0 = p ? p[0]  : 0xFFu;
            const uint8_t p1 = p ? p[-1] : 0xFFu;

            row_buf[b] = (uint8_t)(
                (lut[(uint8_t)((p0 << 4)    | (n0 & 0x0Fu))] << 6) |
                (lut[(uint8_t)((p0 & 0xF0u) | (n0 >> 4))]   << 4) |
                (lut[(uint8_t)((p1 << 4)    | (n1 & 0x0Fu))] << 2) |
                 lut[(uint8_t)((p1 & 0xF0u) | (n1 >> 4))]);
            n -= 2;
            if (p) p -= 2;
        }
    } else {
        const uint8_t *n = next_row;
        const uint8_t *p = prev_row;
        for (int b = 0; b < row_out_bytes; b++) {
            const uint8_t n0 = n[0], n1 = n[1];
            const uint8_t p0 = p ? p[0] : 0xFFu;
            const uint8_t p1 = p ? p[1] : 0xFFu;

            row_buf[b] = (uint8_t)(
                (lut[(uint8_t)((p0 & 0xF0u) | (n0 >> 4))]   << 6) |
                (lut[(uint8_t)((p0 << 4)    | (n0 & 0x0Fu))] << 4) |
                (lut[(uint8_t)((p1 & 0xF0u) | (n1 >> 4))]   << 2) |
                 lut[(uint8_t)((p1 << 4)    | (n1 & 0x0Fu))]);
            n += 2;
            if (p) p += 2;
        }
    }
}

static esp_err_t send_frame_du(epd_panel_handle_t  panel,
                                epd_display_priv_t *priv,
                                const uint8_t      *prev_buf,
                                const uint8_t      *image_buf,
                                uint8_t             dark_threshold,
                                int                 parity,
                                const epd_window_t *win)
{
    epd_i80_bus_handle_t bus   = epd_panel_get_bus(panel);
    const epd_panel_def_t *def = priv->def;
    const size_t row_src_bytes = def->width / 2;
    const bool mirror_y = (priv->flags & EPD_PANEL_FLAG_MIRROR_Y) != 0;
    const bool mirror_x = (priv->flags & EPD_PANEL_FLAG_MIRROR_X) != 0;
    const bool partial  = !window_is_full(def, win);

    du_build_diff_lut(priv, dark_threshold);

    /* Everything outside the window is "not driven" too, so it alternates with
     * the same parity - see du_build_diff_lut(). */
    const uint8_t idle_fill = code_fill_byte(parity ? def->codes.hold
                                                    : def->codes.no_drive);

    epd_row_pipe_t p;
    row_pipe_begin(&p, bus, priv);

    for (uint16_t y = 0; y < def->height; y++) {
        const uint16_t src_y = mirror_y ? (def->height - 1u - y) : y;
        uint8_t *row_buf = row_pipe_buf(&p);

        /* Build unconditionally so per-row timing stays uniform — see the note
         * in send_frame_gc16(). */
        build_row_du(priv, row_buf,
                     prev_buf ? prev_buf + (size_t)src_y * row_src_bytes : NULL,
                     image_buf + (size_t)src_y * row_src_bytes,
                     mirror_x, parity);

        /*
         * Outside the window the pixels are HELD, not grounded.
         *
         * A window is typically a small part of the panel - a few tens of rows
         * out of 1404 - so if the remainder is grounded then almost every pixel
         * on the glass sits under a VCOM bias for the whole update, and a white
         * background visibly darkens as successive updates run.  Holding leaves
         * them alone instead.
         */
        if (src_y < win->row_lo || src_y >= win->row_hi) {
            memset(row_buf, idle_fill, priv->row_bytes);
        } else if (partial) {
            mask_row_columns(row_buf, row_out_bytes_of(def),
                             win->col_lo, win->col_hi, idle_fill);
        }

        esp_err_t ret = row_pipe_submit(&p);
        if (ret != ESP_OK) return row_pipe_abort(&p, ret);
    }
    return row_pipe_finish(&p);
}

/*******************************************************************************
 * Waveform sequences
 ******************************************************************************/

/*
 * INIT — clears display ghosting.
 * Alternates black / white polarity groups twice.
 * ENDS ON WHITE (VPOS) so the screen is fully WHITE going into DU/GC16.
 *
 * Each group is repeated according to the temperature scale, so the black and
 * white halves stay balanced and the sequence still ends white.
 *
 * COST: refresh time = HEIGHT * frames * row_period, and each pixel is driven
 * only during its own gate slot, so the drive energy a pixel receives is
 * frames * row_period.  Refresh time is therefore directly proportional to
 * drive energy — there is no way to make INIT faster without giving the
 * pixels less drive.
 *
 * At init_group_frames = 4 that is 16 frames x ~52 us = 832 us of drive per
 * pixel and ~1.18 s per clear, which is 77 % of a full INIT+GC16 update.
 *
 * MEASURED ON HARDWARE (see wf.init_group_frames in epd_panels.c):
 * 2 (8 frames, 416 us of drive) does NOT fully clear —
 * the previous image ghosts through the background.  4 is clean.  The panel
 * therefore needs somewhere between 416 and 832 us here, so this is close to
 * its floor and INIT is drive-limited rather than overhead-limited: no amount
 * of software optimisation will make a full clear meaningfully faster.
 *
 * The way to avoid paying it is not to clear at all — drive the actual
 * transition instead via a differential DU update, and reserve INIT for
 * periodic de-ghost.
 */
static esp_err_t waveform_init(epd_panel_handle_t panel, epd_display_priv_t *priv)
{
    /* Polarity of each group; ends on WF_BLACK (VPOS) so the screen is WHITE. */
    const epd_drive_codes_t *codes = &priv->def->codes;
    const uint8_t group_polarity[] = {
        codes->darken,    /* VNEG -> black */
        codes->lighten,   /* VPOS -> white */
        codes->darken,    /* VNEG -> black */
        codes->lighten,   /* VPOS -> white <- ends WHITE */
    };
    const int repeat = scale_frames(priv, priv->def->wf.init_group_frames);

    for (int i = 0; i < (int)(sizeof(group_polarity) / sizeof(group_polarity[0])); i++) {
        for (int r = 0; r < repeat; r++) {
            ESP_RETURN_ON_ERROR(
                send_frame_uniform(panel, priv, wf_uniform(group_polarity[i])),
                TAG, "INIT frame %d.%d failed", i, r);
        }
    }
    return ESP_OK;
}

/*
 * DU (Direct Update) — 2-level black/white
 *
 * With prev_buf == NULL every pixel is treated as starting from INIT's white
 * baseline: pixels with a target level below the threshold get VNEG (black
 * drive), everything else gets no-drive and stays white. This direction-blind
 * model cannot ever lighten a pixel, which matters once the caller's actual
 * baseline is not pure white - see du_build_diff_lut().
 *
 * With prev_buf non-NULL each pixel is driven by its real transition: target
 * below threshold -> darken; target above threshold but the pixel is
 * currently below it -> actively lighten (VPOS) instead of being left alone;
 * otherwise it was already correct and gets the bias-cancelling idle code.
 * This is what lets a partial update erase old content instead of only ever
 * adding ink on top of it.
 *
 * 8 frames at nominal temperature gives a reliable transition without
 * overdrive column ringing.
 */
static esp_err_t waveform_du(epd_panel_handle_t  panel,
                               epd_display_priv_t *priv,
                               const uint8_t      *prev_buf,
                               const uint8_t      *next_buf,
                               const epd_window_t *win)
{
    /*
     * Rounded up to an even count.
     *
     * Undriven pixels alternate between the two non-neutral idle codes so their
     * bias cancels (see du_build_diff_lut()), which only works out exactly over
     * a whole number of pairs.  Temperature scaling can otherwise land on an
     * odd count - 8 frames becomes 7 at 85% - leaving one frame's worth of
     * drift behind on every update.
     */
    int frames = scale_frames(priv, priv->def->wf.du_frames);
    if (frames & 1) {
        frames++;
    }

    for (int i = 0; i < frames; i++) {
        ESP_RETURN_ON_ERROR(
            send_frame_du(panel, priv, prev_buf, next_buf,
                          priv->def->wf.du_dark_threshold, i & 1, win),
            TAG, "DU frame %d failed", i);
    }
    return ESP_OK;
}

/*
 * GC16 — 16-level greyscale update.
 *
 * The screen must be at the white baseline (as left by waveform_init): each
 * phase can only darken, so grey levels are built up by how many of the
 * GC16_PHASES frames drive a pixel.
 *
 * An 8x8 Bayer offset dithers the phase thresholds to synthesise 16 levels
 * from the panel's few reliable temporal tones.  When temperature compensation
 * asks for more energy, each phase frame is repeated rather than adding new
 * phases, which preserves the tone mapping.
 */
static esp_err_t waveform_gc16(epd_panel_handle_t  panel,
                                epd_display_priv_t *priv,
                                const uint8_t      *next_buf,
                                const epd_window_t *win)
{
    const int repeat = scale_frames(priv, 1);

    for (int phase = 0; phase < priv->def->wf.gc16_phases; phase++) {
        for (int r = 0; r < repeat; r++) {
            ESP_RETURN_ON_ERROR(
                send_frame_gc16(panel, priv, next_buf, phase, win),
                TAG, "GC16 phase %d.%d failed", phase, r);
        }
    }
    return ESP_OK;
}

/*******************************************************************************
 * Panel ops — implementations of epd_panel_ops_t
 ******************************************************************************/

static esp_err_t epd_display_init(epd_panel_handle_t panel)
{
    const epd_panel_config_t *cfg = epd_panel_get_config(panel);
    const epd_panel_def_t *def = (const epd_panel_def_t *)cfg->driver_cfg;

    ESP_RETURN_ON_ERROR(epd_panel_def_validate(def), TAG,
                        "invalid panel definition");

    epd_display_priv_t *priv = calloc(1, sizeof(*priv));
    ESP_RETURN_ON_FALSE(priv, ESP_ERR_NO_MEM, TAG, "priv alloc failed");

    /* Own the definition outright; see epd_display_priv_t::def. */
    priv->def_storage = *def;
    priv->def         = &priv->def_storage;
    priv->row_bytes   = (size_t)def->width / 4;
    priv->flags       = cfg->flags;
    priv->temp_comp   = cfg->temp_compensation;
    priv->temp_c      = 25;    /* assume nominal until the PMIC is powered */
    priv->frame_pct   = 100;

    /*
     * Two DMA row buffers so the CPU can build row N+1 while the DMA engine is
     * still reading row N.
     *
     * Each holds active bytes + 16-byte line padding (sent as dummy clocks),
     * rounded up to a whole 64-byte cache line.  On the ESP32-P4 internal SRAM
     * is reached through L1 cache, so a partial trailing line would let a
     * writeback of this buffer touch whatever allocation shares that line.
     * Zeroed so the padding bytes are 0x00.
     */
    priv->row_alloc = (priv->row_bytes + def->line_padding_bytes + 63u) & ~(size_t)63u;

    for (int i = 0; i < 2; i++) {
        priv->row_buf[i] = heap_caps_aligned_alloc(64, priv->row_alloc,
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!priv->row_buf[i]) {
            for (int j = 0; j < i; j++) {
                heap_caps_free(priv->row_buf[j]);
            }
            free(priv);
            return ESP_ERR_NO_MEM;
        }
        memset(priv->row_buf[i], 0, priv->row_alloc);  /* zero padding bytes */
    }

    priv->du_diff_lut_threshold = -1;  /* built on the first DU frame         */
    priv->gc16_lut_phase      = -1;    /* force a LUT rebuild on first frame  */

    epd_panel_set_priv(panel, priv);
    ESP_LOGI(TAG, "init OK  \"%s\"  %ux%u  %u-bit  row_bytes=%zu (%zu alloc x2)  "
                  "row_period=%u us  gc16_phases=%u  init_frames=%u  temp_comp=%s",
             def->name, def->width, def->height, def->bus_width,
             priv->row_bytes, priv->row_alloc,
             def->timing.row_period_us, def->wf.gc16_phases,
             (unsigned)(def->wf.init_group_frames * 4u),
             priv->temp_comp ? "on" : "off");
    return ESP_OK;
}

static esp_err_t epd_display_deinit(epd_panel_handle_t panel)
{
    epd_display_priv_t *priv = epd_panel_get_priv(panel);
    if (priv) {
        /*
         * Drain before freeing: epd_panel_destroy() runs ops->deinit ahead of
         * epd_i80_bus_deinit(), so this is the last point at which an in-flight
         * DMA can still be reading out of row_buf[].
         */
        epd_i80_bus_wait_row(epd_panel_get_bus(panel));

        heap_caps_free(priv->row_buf[0]);
        heap_caps_free(priv->row_buf[1]);
        free(priv);
        epd_panel_set_priv(panel, NULL);
    }
    return ESP_OK;
}

static esp_err_t epd_display_power_on(epd_panel_handle_t panel)
{
    epd_i80_bus_handle_t     bus  = epd_panel_get_bus(panel);
    tps65185_handle_t        pmic = epd_panel_get_pmic(panel);
    const epd_panel_config_t *cfg = epd_panel_get_config(panel);
    epd_display_priv_t      *priv = epd_panel_get_priv(panel);

    /* Re-enable source outputs and gate driver mode before powering rails */
    epd_i80_oe_enable(bus);              /* source outputs enabled */
    epd_i80_gmod_set(bus, true);         /* GMOD / EP_MODE on      */

    ESP_RETURN_ON_ERROR(tps65185_wakeup(pmic), TAG, "PMIC wakeup failed");
    vTaskDelay(pdMS_TO_TICKS(10));   /* allow oscillator to stabilise */

    /*
     * Program the panel's rail ordering before powering up.  Panels specify
     * the order their source and gate rails must appear in, so this belongs to
     * the panel definition rather than to the PMIC driver, which previously
     * hardcoded one panel's ordering for every panel.
     */
    if (priv) {
        const epd_power_seq_t *ps = &priv->def->power;
        esp_err_t sret = tps65185_set_powerup_sequence(
            pmic, TPS65185_REG_UPSEQ0,
            (tps65185_strobe_t)ps->vddh_strobe, (tps65185_strobe_t)ps->vpos_strobe,
            (tps65185_strobe_t)ps->vee_strobe,  (tps65185_strobe_t)ps->vneg_strobe);
        if (sret != ESP_OK) {
            ESP_LOGW(TAG, "power-up sequence write failed: %s", esp_err_to_name(sret));
        }

        /* UPSEQ1 packs four 2-bit delay fields: 0=3ms, 1=6ms, 2=9ms, 3=12ms. */
        uint8_t step = ps->delay_per_strobe_ms;
        uint8_t code = (step >= 12) ? 3u : (step >= 9) ? 2u : (step >= 6) ? 1u : 0u;
        uint8_t upseq1 = (uint8_t)((code << 6) | (code << 4) | (code << 2) | code);
        sret = tps65185_write_register(pmic, TPS65185_REG_UPSEQ1, upseq1);
        if (sret != ESP_OK) {
            ESP_LOGW(TAG, "power-up delay write failed: %s", esp_err_to_name(sret));
        }
    }

    ESP_RETURN_ON_ERROR(tps65185_set_vcom(pmic, cfg->vcom_mv),
                        TAG, "set_vcom failed");
    ESP_RETURN_ON_ERROR(tps65185_power_up(pmic), TAG, "power_up failed");

    /*
     * Wait for the rails, rather than sampling once after a fixed delay.
     *
     * The TPS65185 raises its rails in a programmed sequence with a delay
     * between strobes, so any fixed delay races the sequencer.  Measured on
     * this board: VPOS/VEE/VNEG are good ~150 ms after PWRUP but VDDH not
     * until ~300 ms.  A single read at 50 ms therefore reported a missing gate
     * rail on a panel that was about to work perfectly - an alarming error for
     * a healthy board, and one that would mask a real fault by crying wolf.
     */
    const uint8_t rails = TPS65185_PG_VB_PG   | TPS65185_PG_VDDH_PG |
                          TPS65185_PG_VN_PG   | TPS65185_PG_VPOS_PG |
                          TPS65185_PG_VEE_PG  | TPS65185_PG_VNEG_PG;
    uint8_t   pg     = 0;
    esp_err_t pg_ret = ESP_FAIL;
    int       waited = 0;

    for (;;) {
        pg_ret = tps65185_get_power_good_status(pmic, &pg);
        if (pg_ret == ESP_OK && (pg & rails) == rails) {
            break;
        }
        if (waited >= EPD_PG_TIMEOUT_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(EPD_PG_POLL_MS));
        waited += EPD_PG_POLL_MS;
    }

    /*
     * Report the rails rather than gate on them.
     *
     * A missing individual rail after the timeout above is a real fault and is
     * warned about, because a panel whose rails never came up looks exactly
     * like one that works and simply shows nothing - which is what this log is
     * here to distinguish.  PG_ALL alone is not treated as authoritative: on
     * this board it can stay clear while every individual rail reads good.
     */
    if (pg_ret != ESP_OK) {
        ESP_LOGW(TAG, "power-good read failed: %s", esp_err_to_name(pg_ret));
    } else {
        if ((pg & rails) != rails) {
            ESP_LOGE(TAG, "PMIC rails NOT good after %d ms: PG=0x%02X "
                          "(VB=%d VDDH=%d VN=%d VPOS=%d VEE=%d VNEG=%d) — "
                          "the panel will show nothing if a source or gate rail is missing",
                     waited, pg,
                     !!(pg & TPS65185_PG_VB_PG),   !!(pg & TPS65185_PG_VDDH_PG),
                     !!(pg & TPS65185_PG_VN_PG),   !!(pg & TPS65185_PG_VPOS_PG),
                     !!(pg & TPS65185_PG_VEE_PG),  !!(pg & TPS65185_PG_VNEG_PG));
        } else {
            ESP_LOGI(TAG, "PMIC rails good after %d ms: PG=0x%02X", waited, pg);
        }
    }

    ESP_RETURN_ON_ERROR(tps65185_vcom_enable(pmic, true),
                        TAG, "vcom_enable failed");

    /*
     * Sample the panel temperature now that the PMIC is awake, and hold the
     * resulting frame scale for every refresh until the next power cycle.
     * A failed read is not fatal — fall back to the nominal tuning.
     */
    if (priv) {
        priv->frame_pct = 100;
        if (priv->temp_comp) {
            int8_t t = 0;
            esp_err_t tret = tps65185_read_temperature(pmic, &t);
            if (tret != ESP_OK) {
                ESP_LOGW(TAG, "temperature read failed (%s), using nominal timing",
                         esp_err_to_name(tret));
            } else if (t < TEMP_VALID_MIN_C || t > TEMP_VALID_MAX_C) {
                ESP_LOGW(TAG, "implausible temperature %d C (thermistor fault?), "
                              "using nominal timing", t);
            } else {
                priv->temp_c    = t;
                priv->frame_pct = temp_frame_pct(t);
                ESP_LOGI(TAG, "panel temperature %d C → frame scale %u%%",
                         t, priv->frame_pct);
            }
        }
    }

    ESP_LOGI(TAG, "power on OK  vcom=%u mV", cfg->vcom_mv);
    return ESP_OK;
}

/*
 * Open EVERY gate row simultaneously.
 *
 * SPV is active-low and injects a token on each CKV edge while asserted, so
 * holding it low for a full panel's worth of clocks fills the gate shift
 * register with tokens and selects all 1404 rows at once.
 *
 * With the source lines held at ground (OE enabled, an all-no-drive row
 * latched) and VCOM already pulled down, every pixel is then connected to
 * ground in parallel and drains its residual charge simultaneously.  There is
 * no voltage difference anywhere on the panel, so nothing is driven and no
 * appreciable current flows — this is a discharge, not a drive.
 *
 * That residual charge is what makes the image creep after power-off: each
 * pixel holds a slightly different leftover bias, so they relax by slightly
 * different amounts, which reads as graininess.
 */
static void gate_all_on(epd_i80_bus_handle_t bus, const epd_panel_def_t *def)
{
    epd_i80_spv_low(bus);       /* asserted: inject a token on every clock */

    for (uint16_t i = 0; i < def->height; i++) {
        epd_i80_ckv_low(bus);
        esp_rom_delay_us(def->timing.ckv_flush_us);
        epd_i80_ckv_high(bus);
        esp_rom_delay_us(def->timing.ckv_flush_us);
    }

    epd_i80_spv_high(bus);      /* stop injecting; rows stay selected */
}

/*
 * Push every gate row into the OFF state and leave it there.
 *
 * SPV is held de-asserted so no new row token can be injected, then CKV is
 * clocked enough times to shift any token still sitting in the gate shift
 * register off the end.  Afterwards no row is selected, so the gate driver
 * actively holds every gate at VGL and all pixels are isolated.
 *
 * This matters during power-down: if a token is left in the register, that one
 * row stays connected to its source line while the rails decay.  Flushing is
 * cheap insurance (~8 ms) and needs no particular timing accuracy, so it runs
 * outside a critical section.
 */
static void gate_flush_all_off(epd_i80_bus_handle_t bus, const epd_panel_def_t *def)
{
    epd_i80_spv_high(bus);      /* de-asserted: no token injection */

    for (uint16_t i = 0; i < def->height; i++) {
        epd_i80_ckv_low(bus);
        esp_rom_delay_us(def->timing.ckv_flush_us);
        epd_i80_ckv_high(bus);
        esp_rom_delay_us(def->timing.ckv_flush_us);
    }

    epd_i80_ckv_low(bus);       /* leave the gate clock idle low */
}

static esp_err_t epd_display_power_off(epd_panel_handle_t panel)
{
    epd_i80_bus_handle_t bus  = epd_panel_get_bus(panel);
    tps65185_handle_t    pmic = epd_panel_get_pmic(panel);
    epd_display_priv_t  *priv = epd_panel_get_priv(panel);

    /*
     * The whole sequence below needs the panel definition for its timings, so
     * bail out rather than carrying a NULL check past the first use of it.
     */
    ESP_RETURN_ON_FALSE(priv, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    const epd_ac_timing_t *t = &priv->def->timing;

    /*
     * Order matters:
     *   1. VCOM -> 0 V   — common plane ACTIVELY driven to ground (not disabled,
     *                      which would float it)
     *   2. discharge     — per-row neutralising scan, now at zero bias
     *   3. global drain  — ALL gate rows open at once, held, so every pixel
     *                      storage node bleeds to ground in parallel
     *   4. gate flush    — every gate row actively held OFF, pixels isolated
     *   5. power_down    — rails collapse with sources, gates and common plane
     *                      all still HELD at a known potential
     *   6. release       — let go of OE / GMOD, but NOT VCOM: the common plane
     *                      has no bleed path on this board and must stay held
     *   7. standby/sleep
     *
     * The neutralising scan MUST come AFTER VCOM is at ground.
     *
     * The source driver's "no-drive" code leaves the source lines near ground.
     * While VCOM is still live at its operating voltage that is NOT neutral —
     * every pixel sees a DC field of the full VCOM magnitude for the entire
     * scan, which visibly darkens and speckles the image right as the panel
     * shuts down.  The bias is proportional to VCOM, so this stays invisible on
     * a mis-set near-zero VCOM and only appears once VCOM is correct.
     *
     * Equally important: the source outputs stay ENABLED through the rail
     * collapse, holding every source line actively at ground.  Tri-stating them
     * first leaves the lines floating with residual charge, and as VGL decays
     * the TFTs begin to conduct and share that charge into the pixels — which
     * is precisely the speckle this sequence is trying to avoid.  Ground is
     * also where VCOM already is, so the pixels see no field either way.
     */

    /* 1. Ramp VCOM to 0 V but keep the buffer ENABLED, so the common plane is
     *    ACTIVELY held at ground for the whole shutdown.
     *
     *    Do not simply disable VCOM here: tps65185_vcom_enable(false) drops
     *    VCOM_CTRL, which turns the VCOM buffer off and leaves the common
     *    electrode high-impedance.  A floating common plane sitting against
     *    charged pixels drifts to wherever leakage takes it and imposes an
     *    uncontrolled field on every pixel while the rails collapse — which is
     *    exactly the post-shutdown speckle we are trying to remove.  (Setting
     *    VCOM Hi-Z has the same problem, which is why that was rejected too.)
     *
     *    Driving it to 0 V instead puts the common plane at the same potential
     *    as the grounded source lines, so every pixel sees zero field. */
    esp_err_t ret = tps65185_set_vcom(pmic, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VCOM ramp to 0 V failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(t->vcom_off_settle_ms));

    /* 2. Neutralising scan at zero bias.  Leaves an all-no-drive row latched,
     *    so every source line ends up held at ground. */
    for (int i = 0; i < t->discharge_frames; i++) {
        (void)send_frame_uniform(panel, priv,
                                 wf_uniform(priv->def->codes.no_drive));
    }

    /* 3. Global discharge: open every row at once and hold, so all pixel
     *    storage nodes drain to ground in parallel.  This is the step that
     *    actually removes the per-pixel residual bias responsible for the
     *    image creeping after power-off. */
    gate_all_on(bus, priv->def);
    vTaskDelay(pdMS_TO_TICKS(t->global_discharge_ms));

    /* 4. Isolate every pixel: no gate row selected, all gates driven to VGL. */
    gate_flush_all_off(bus, priv->def);
    /* 5. Collapse the rails.  OE stays ENABLED, GMOD stays ON and the VCOM
     *    buffer stays ENABLED at 0 V here on purpose — see the notes above — so
     *    source lines, gates and the common plane are all actively held at a
     *    known potential for the whole decay.  The VCOM buffer loses its supply
     *    as part of the sequenced power-down, by which point every node on the
     *    panel is already at ground. */
    ret = tps65185_power_down(pmic);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pmic power_down failed: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(t->post_discharge_wait_ms));

    /* 6. Rails are down and the drivers are unpowered; releasing the control
     *    lines can no longer move any charge. */
    epd_i80_oe_disable(bus);
    epd_i80_gmod_set(bus, false);

    /*
     * The VCOM buffer is deliberately LEFT ENABLED.
     *
     * tps65185_vcom_enable(false) drops VCOM_CTRL and leaves the common
     * electrode high impedance.  On this board VCOM has no bleed path to
     * ground - the net is a 4.7 uF capacitor, the PMIC pin and the panel, and
     * nothing else - so disabling the buffer strands that capacitor at
     * whatever charge it holds with nowhere to drain.  It then drifts on
     * leakage from the pixels it faces, and because it is the common plane
     * that drift is a slowly growing field across every pixel on the panel.
     * A held white image goes grainy over minutes.
     *
     * This is the same failure the sequence above avoids during the rail
     * collapse; releasing VCOM here simply moved it into the idle period,
     * where the panel spends far longer.  Keeping VCOM_CTRL asserted holds the
     * plane at the 0 V programmed in step 1 instead.
     */

    /*
     * 7. Latch the STANDBY bit before the I2C interface goes away.
     *
     * Deliberately NOT followed by a sleep here.  Sleep is entered by dropping
     * the PMIC's WAKEUP pin, which powers down its I2C interface entirely -
     * that is a whole-system power decision belonging to the application, not
     * something a display driver should do behind its back.  Call
     * tps65185_sleep() after this returns if the PMIC has nothing else to do.
     */
    ret = tps65185_standby(pmic);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pmic standby failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

static void epd_report_row_stats(const epd_display_priv_t *priv, const char *what)
{
#if EPD_ROW_STATS
    const epd_row_stats_t *s = &priv->stats;
    if (s->rows == 0) return;

    const int64_t r = s->rows;

    /*
     * build + wait + submit account for the whole row period.
     *
     * dma and wake are a separate breakdown of the transfer itself and are
     * deliberately NOT part of that sum: when the row build is longer than the
     * transfer, the DMA runs entirely inside the build and contributes nothing
     * to wait.  Comparing dma against wait is what shows the pipeline working —
     * a large dma with a near-zero wait means the transfer is fully hidden.
     */
    ESP_LOGI(TAG,
             "%s: %u frames, %u rows, %lld ms | %lld us/row = "
             "build %lld + wait %lld + submit %lld | dma %lld, wake %lld "
             "| overruns %u",
             what, s->frames, s->rows, (long long)(s->busy_us / 1000),
             (long long)(s->busy_us   / r),
             (long long)(s->build_us  / r),
             (long long)(s->wait_us   / r),
             (long long)(s->submit_us / r),
             (long long)(s->dma_us    / r),
             (long long)(s->wake_us   / r),
             s->overruns);
#else
    (void)priv; (void)what;
#endif
}

static esp_err_t epd_display_refresh(epd_panel_handle_t  panel,
                                   const void         *prev_buf,
                                   const void         *next_buf,
                                   epd_waveform_mode_t mode,
                                   const epd_rect_t   *area)
{
    epd_display_priv_t *priv = epd_panel_get_priv(panel);
    ESP_RETURN_ON_FALSE(priv, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    const bool mirror_x = (priv->flags & EPD_PANEL_FLAG_MIRROR_X) != 0;

    epd_window_t win;
    if (area) {
        window_from_rect(priv->def, area, mirror_x, &win);
    } else {
        window_full(priv->def, &win);
    }

    memset(&priv->stats, 0, sizeof(priv->stats));
    esp_err_t ret;

    switch (mode) {
        case EPD_WAVEFORM_INIT:
            /* Always clears the whole panel; area is not supported for INIT. */
            if (area) {
                ESP_LOGW(TAG, "EPD_WAVEFORM_INIT ignores area; clearing whole panel");
            }
            ret = waveform_init(panel, priv);
            epd_report_row_stats(priv, "INIT");
            return ret;

        case EPD_WAVEFORM_DU:
        case EPD_WAVEFORM_A2:
            ret = waveform_du(panel, priv, (const uint8_t *)prev_buf,
                              (const uint8_t *)next_buf, &win);
            epd_report_row_stats(priv, prev_buf ? "DU(diff)" : "DU");
            return ret;

        case EPD_WAVEFORM_GC16:
        case EPD_WAVEFORM_GL16:
        default:
            if (prev_buf) {
                ESP_LOGW(TAG, "EPD_WAVEFORM_GC16/GL16 ignores prev_buf; driving absolute levels");
            }
            ret = waveform_gc16(panel, priv, (const uint8_t *)next_buf, &win);
            epd_report_row_stats(priv, "GC16");
            return ret;
    }
}

/*******************************************************************************
 * Public entry point
 ******************************************************************************/

static const epd_panel_ops_t epd_display_ops = {
    .init      = epd_display_init,
    .deinit    = epd_display_deinit,
    .power_on  = epd_display_power_on,
    .power_off = epd_display_power_off,
    .refresh   = epd_display_refresh,
};

/*******************************************************************************
 * Board wiring -> peripheral pin order
 *
 * The LCD peripheral emits the FIRST byte of every transfer on its data signals
 * 0..7 and the second on 8..15, with no notion of which lines the panel calls
 * D0-D7.  epd_board_config_t is expressed in the panel's numbering, so this is
 * where the two are reconciled.
 *
 * Getting it wrong on an 8-bit panel is silent and total: the peripheral drives
 * lines the panel does not have, its real inputs stay at 0x00 which decodes as
 * "no drive", and the screen shows nothing at all - not even the INIT flash.
 * Two 8-bit panels were blank for exactly this reason while two 16-bit panels
 * worked, because a 16-bit panel drives all sixteen lines and so cannot reveal
 * which physical half is D0-D7.
 *
 * Mirroring, however, does NOT follow from bus width - an inference that looked
 * safe on the first four panels and was then refuted by the next three.  Both
 * widths have panels needing MIRROR_X and panels needing NONE, so flags has to
 * be settled per panel by looking at the screen.
 ******************************************************************************/
static esp_err_t board_to_bus_cfg(const epd_board_config_t *b,
                                   uint8_t                  bus_width,
                                   epd_i80_bus_config_t    *out)
{
    memset(out, 0, sizeof(*out));

    for (int i = 0; i < EPD_BOARD_DATA_PINS; i++) {
        out->data_pins[i] = GPIO_NUM_NC;
    }

    if (bus_width == 8) {
        /* The panel only has D0-D7, so they must occupy entries 0..7. */
        for (int i = 0; i < 8; i++) {
            ESP_RETURN_ON_FALSE(b->data[i] >= 0, ESP_ERR_INVALID_ARG, TAG,
                                "board.data[%d] is not a valid GPIO", i);
            out->data_pins[i] = b->data[i];
        }
    } else {
        for (int i = 0; i < EPD_BOARD_DATA_PINS; i++) {
            ESP_RETURN_ON_FALSE(b->data[i] >= 0, ESP_ERR_INVALID_ARG, TAG,
                                "board.data[%d] is not a valid GPIO", i);
        }
        const int lo = b->bus16_low_byte_first ? 0 : 8;   /* first byte's half */
        const int hi = b->bus16_low_byte_first ? 8 : 0;
        for (int i = 0; i < 8; i++) {
            out->data_pins[i]     = b->data[lo + i];
            out->data_pins[8 + i] = b->data[hi + i];
        }
    }

    out->pin_cl       = b->cl;
    out->pin_le       = b->le;
    out->pin_oe       = b->oe;
    out->pin_sph      = b->sph;
    out->pin_spv      = b->spv;
    out->pin_ckv      = b->ckv;
    out->pin_gmod     = b->gmod;
    out->pin_dc_dummy = b->dc_dummy;
    out->oe_active_high = b->oe_active_high;

    /*
     * IDF rejects a negative DC pin outright, and the failure surfaces from
     * deep inside esp_lcd_new_i80_bus() as a bare ESP_ERR_INVALID_ARG.
     */
    ESP_RETURN_ON_FALSE(b->dc_dummy >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "board.dc_dummy must be a real GPIO: the i80 peripheral "
                        "requires a DC pin even though an EPD has no DC line");
    ESP_RETURN_ON_FALSE(b->cl >= 0 && b->le >= 0 && b->sph >= 0 &&
                        b->spv >= 0 && b->ckv >= 0,
                        ESP_ERR_INVALID_ARG, TAG,
                        "board cl/le/sph/spv/ckv must all be valid GPIOs");
    return ESP_OK;
}

esp_err_t epd_display_panel_create(const epd_panel_def_t    *def,
                                   const epd_board_config_t *board,
                                   tps65185_handle_t         pmic,
                                   epd_panel_handle_t       *handle)
{
    ESP_RETURN_ON_FALSE(def && board && pmic && handle, ESP_ERR_INVALID_ARG,
                        TAG, "NULL arg");
    ESP_RETURN_ON_ERROR(epd_panel_def_validate(def), TAG,
                        "invalid panel definition");

    epd_panel_config_t cfg = {
        .width     = def->width,
        .height    = def->height,
        .bus_width = def->bus_width,
        .vcom_mv   = def->vcom_mv,
        .flags     = def->flags,
        .pmic      = pmic,
        .temp_compensation = def->temp_compensation,
        .driver_cfg        = def,
    };

    ESP_RETURN_ON_ERROR(board_to_bus_cfg(board, def->bus_width, &cfg.bus_cfg),
                        TAG, "bad board configuration");

    /* Log the pins actually driven, so they can be checked against the board. */
    ESP_LOGI(TAG, "%s: %u-bit bus on GPIO %d,%d,%d,%d,%d,%d,%d,%d%s",
             def->name, def->bus_width,
             cfg.bus_cfg.data_pins[0], cfg.bus_cfg.data_pins[1],
             cfg.bus_cfg.data_pins[2], cfg.bus_cfg.data_pins[3],
             cfg.bus_cfg.data_pins[4], cfg.bus_cfg.data_pins[5],
             cfg.bus_cfg.data_pins[6], cfg.bus_cfg.data_pins[7],
             def->bus_width == 16 ? " (+8 more)" : "");
    ESP_LOGI(TAG, "%s: CL=%d LE=%d OE=%d SPH=%d SPV=%d CKV=%d GMOD=%d DC=%d",
             def->name,
             cfg.bus_cfg.pin_cl,  cfg.bus_cfg.pin_le,  cfg.bus_cfg.pin_oe,
             cfg.bus_cfg.pin_sph, cfg.bus_cfg.pin_spv, cfg.bus_cfg.pin_ckv,
             cfg.bus_cfg.pin_gmod, cfg.bus_cfg.pin_dc_dummy);

    cfg.bus_cfg.pclk_hz = def->pclk_hz;
    /* max bytes per transfer = active (width/4) + the panel's line padding */
    cfg.bus_cfg.max_row_bytes = (size_t)def->width / 4 + def->line_padding_bytes;

    return epd_panel_create(&cfg, &epd_display_ops, handle);
}
