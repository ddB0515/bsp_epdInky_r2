#ifndef EPD_PANEL_DEF_H
#define EPD_PANEL_DEF_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Panel definition
 *
 * Everything here describes the PANEL: its geometry, the AC timing its gate and
 * source drivers require, and the waveform/tone model tuned for its ink.  A
 * definition is passed to epd_display_panel_create() and the driver reads all
 * of it at runtime, so supporting another panel means adding a definition in
 * epd_panels.c rather than editing the driver.
 *
 * What is deliberately NOT here is board wiring.  GPIO assignments belong to
 * the board, not the panel: swap the panel on the same board and the pins are
 * unchanged, move the same panel to another board and they all differ.  The
 * pin map therefore lives with the board configuration in epd_display.c.
 *
 * Values are per-panel and generally NOT portable between panel types.  The
 * waveform block in particular is empirically tuned - see the notes in
 * epd_display.c on how the current numbers were arrived at, and on which
 * experiments failed.
 ******************************************************************************/

/** Panel orientation / mirror flags (combinable with |). */
#define EPD_PANEL_FLAG_NONE     0x00u   /**< No mirroring                       */
#define EPD_PANEL_FLAG_MIRROR_X 0x01u   /**< Flip horizontal (left<->right)     */
#define EPD_PANEL_FLAG_MIRROR_Y 0x02u   /**< Flip vertical   (top<->bottom)     */

/** Upper bound on gc16_phases, sizing the fixed phase arrays below. */
#define EPD_GC16_MAX_PHASES  8

/*
 * Source-driver AC timing, all in microseconds unless named _ms.
 *
 * These are the bit-banged gate/latch intervals the driver places around each
 * DMA row transfer.  They are timing-critical: the frame start sequence runs
 * with interrupts disabled because an ISR landing between two edges shifts the
 * gate start and offsets the whole frame by a row.
 */
typedef struct {
    /* Frame gate start: positions the gate driver at row 0. */
    uint16_t ckv_pre_spv_us;    /**< CKV high before SPV is asserted low       */
    uint16_t spv_low_us;        /**< SPV asserted (low) duration               */
    uint16_t ckv_post_spv_us;   /**< CKV high after SPV deassert               */
    uint16_t spv_high_us;       /**< SPV de-asserted (high) duration           */
    uint16_t ckv_extra_us;      /**< extra CKV half-period to reach row 0      */

    /**
     * Frame sync length, in CKV lines (datasheet t1).
     *
     * How many CKV pulses are clocked while SPV/STV is asserted at the start of
     * a frame, which is what the gate driver samples to know where row 0 is.
     * Most panels here take 1; some specify a minimum of 2 lines, and a frame
     * sync that is too short can leave the gate driver mis-positioned so the
     * whole image is offset vertically or missing entirely.
     *
     * Must be at least 1.  Consecutive pulses are spaced by ckv_extra_us.
     */
    uint8_t spv_sync_lines;

    /**
     * Extra blank CKV lines clocked after the frame sync, before the first
     * row of real data (datasheet FBL / "Frame Blank Length" on panels whose
     * timing diagram names it - most datasheets used so far do not).
     *
     * frame_gate_start() has always clocked a fixed, small number of pulses
     * to walk the gate from the sync assertion to "row 0" - three, on top of
     * spv_sync_lines - which is enough for every panel validated before
     * ED067KC1. That panel's datasheet is the first to give an explicit,
     * much larger figure (64 lines): its gate driver apparently needs a real
     * blanking scan between sync and data, not just a handful of
     * positioning pulses, and with only the fixed three the gate is nowhere
     * near row 0 when real data starts - on a 900-row panel that is a large
     * enough fraction of the frame to show up as exactly the "top shifted,
     * bottom short of the edge" symptom this field was added to fix.
     *
     * 0 (the default for every other panel here) reproduces the old
     * behaviour exactly - see frame_gate_start() in epd_display.c.
     * Consecutive pulses are spaced by ckv_extra_us, same as spv_sync_lines.
     */
    uint16_t frame_blank_lines;

    /**
     * Minimum CKV LOW pulse width (datasheet twL).
     *
     * The gate sequences drive CKV low and high back to back, which without
     * this gives a low pulse only as wide as two GPIO writes - on the order of
     * 100 ns.  Panels that specify a minimum low width need it stretched.
     *
     * 0 keeps the back-to-back behaviour, for panels validated that way.
     * This is applied per row as well as during the frame start, so a non-zero
     * value adds directly to the row period.
     */
    uint16_t ckv_low_us;

    /**
     * Minimum LE (XLE) HIGH pulse width, in microseconds (datasheet tLEw).
     *
     * The driver pulses LE with two back-to-back GPIO writes, which is only
     * some tens of nanoseconds.  That satisfies panels asking for 40 ns, but
     * not ones asking for 300 ns, and a short LE means the row's source data is
     * never reliably latched onto the outputs.
     *
     * 0 keeps the back-to-back behaviour, for panels validated that way.
     * The delay has microsecond granularity, so 1 is the smallest non-zero
     * value and covers any sub-microsecond tLEw with margin.  There is no
     * maximum for tLEw - LE is a level-sensitive latch enable - so erring long
     * is safe; it costs this much time per row.
     */
    uint16_t le_pulse_us;

    uint16_t interframe_us;     /**< settle delay between frames               */

    /*
     * LE-to-LE row period, i.e. how long one row's data is held on the panel
     * outputs.  0 means free-run: the loop goes as fast as the pipeline allows
     * and the period is whatever build/DMA/scheduler add up to.  A non-zero
     * value paces every row to exactly this figure, making the drive dwell a
     * tuning constant rather than a by-product.  See T_ROW_PERIOD notes in
     * epd_display.c for how to choose one.
     */
    uint16_t row_period_us;

    /* Gate clock half-period used by the power-down flush scans. */
    uint16_t ckv_flush_us;

    /* Power-down sequencing. */
    uint16_t vcom_off_settle_ms;      /**< let the common plane reach ground   */
    uint16_t post_discharge_wait_ms;  /**< after the rails are commanded down  */
    uint16_t global_discharge_ms;     /**< all gates open, pixels drain        */
    uint8_t  discharge_frames;        /**< neutralising scans at zero bias     */
} epd_ac_timing_t;

/*
 * Source drive codes.
 *
 * The source bus carries 2 bits per pixel.  Which code drives which way is a
 * property of the panel's source driver IC and must be confirmed per panel -
 * getting it backwards inverts the image.
 *
 * All four encodings are used, and the two non-driving ones are NOT
 * interchangeable:
 *
 *   hold (0b11)      the source line is left alone, so the pixel keeps what it
 *                    has.  This is what an unchanged pixel inside an update
 *                    must get.
 *   no_drive (0b00)  the source line is pulled to ground.  Against a live VCOM
 *                    that is a real field rather than a rest state, so it
 *                    belongs where the whole panel is being settled - the
 *                    discharge scan at power-off, and rows outside an update
 *                    window.
 *
 * Using the grounded code for unchanged pixels applies a bias to them for the
 * length of every update, which shows up as a white background slowly darkening
 * during a partial update.
 */
typedef struct {
    uint8_t no_drive;   /**< source grounded (0b00): settling scans           */
    uint8_t darken;     /**< drives toward black, VNEG  (typically 0b01)      */
    uint8_t lighten;    /**< drives toward white, VPOS  (typically 0b10)      */
    uint8_t hold;       /**< leave the pixel undisturbed (0b11)               */
} epd_drive_codes_t;

/*
 * Waveform and tone model.
 *
 * Grey is synthesised from a small number of temporal phases combined with an
 * 8x8 spatial dither, because the panel resolves far fewer than 16 distinct
 * tones on its own.  A pixel drives in phase p while
 *
 *     level_energy[level] + bayer[y & 7][x & 7]  >=  phase_cut[phase_order[p]]
 *
 * so frame count rises with energy, and the Bayer offset dithers the threshold
 * to fill in between the phases.
 */
typedef struct {
    uint8_t  gc16_phases;                             /**< <= EPD_GC16_MAX_PHASES */
    /**
     * Per-phase thresholds, in the same units as gc16_level_energy plus the
     * Bayer offset.  A phase's cut must exceed the largest Bayer value or that
     * phase drives even at zero energy, so the natural spacing is one cut per
     * (max_bayer + 1): 16/32/48/64 for a 4x4-derived matrix, 64/128/192/256
     * for a full 8x8 one.  Hence uint16_t - 256 does not fit in a byte.
     */
    uint16_t gc16_phase_cut[EPD_GC16_MAX_PHASES];
    uint8_t gc16_phase_order[EPD_GC16_MAX_PHASES];    /**< temporal ordering      */

    /** Darkness energy per 4bpp level: [0] = black .. [15] = white. */
    uint8_t gc16_level_energy[16];

    /**
     * Spatial dither offsets, an 8x8 matrix.
     *
     * This sets the TONE RESOLUTION as well as the dither pattern.  A pixel
     * drives in a phase when energy + bayer >= cut, so over the matrix the
     * fraction of pixels that drive moves in steps of 1/64 and no curve can
     * produce finer gradations than that.
     *
     * Note what that fraction means: greys are DOT DENSITY, not partially
     * darkened pixels.  Until energy reaches the first cut some pixels never
     * drive at all and stay pure white, so an energy of half the first cut is
     * 50% black dots on white - it cannot be black however the panel responds.
     * Solid black needs energy at or above the first cut, and the greys have
     * to be spread over the range below it.
     *
     * Two ways to fill it:
     *
     *   Classic 4x4 dither, tiled 2x2, values 0..15 with cuts 16/32/48/64.
     *   Each Bayer value then appears four times in the 64 cells, so the drive
     *   works out to exactly energy/16 - identical to a true 4x4 matrix.  This
     *   is what the panels tuned before the 8x8 support use, and it keeps their
     *   behaviour bit-for-bit unchanged.
     *
     *   Full 8x8 dither, values 0..63 with cuts 64/128/...  Drive becomes
     *   energy/64, giving four times the tonal resolution.  Worth it on panels
     *   whose usable range is narrow: ED115OC1 reaches black in about 1.5
     *   frames, which at 1/16-frame steps left too few distinct tones for 16
     *   levels and made its highlights collapse onto white.
     */
    uint8_t bayer[8][8];

    /*
     * INIT clears ghosting by alternating black/white polarity groups twice,
     * this many frames per group (4 -> 16 frames).  Refresh time is directly
     * proportional to drive energy, so this is the single biggest cost in the
     * driver - and on the reference panel it is already near its floor.
     */
    uint8_t init_group_frames;

    /** DU frames per update, and the 4bpp level below which a pixel is "dark". */
    uint8_t du_frames;
    uint8_t du_dark_threshold;
} epd_waveform_def_t;

/*
 * PMIC power-up sequencing.
 *
 * Panels specify the order their rails must come up in, and driving them in the
 * wrong order can leave the source or gate drivers latched up.  The strobe
 * fields say which of the four power-up strobes each rail is assigned to, 0 for
 * the first and 3 for the last.
 *
 * Rail naming follows the PMIC; the panel datasheet's VGG is the TPS65185's
 * VDDH.  A typical panel requirement reads as two independent chains, for
 * example "VDD -> VNEG -> VPOS" for the source driver and "VEE -> VGG" for the
 * gate driver, and any strobe assignment that respects both is valid.
 *
 * delay_per_strobe_ms is the gap between consecutive strobes, and must clear
 * the largest inter-rail minimum the panel asks for.  The TPS65185 supports
 * 3, 6, 9 or 12 ms; anything else is rounded down to the next supported step.
 */
typedef struct {
    uint8_t vddh_strobe;            /**< VGG / gate high, 0..3                 */
    uint8_t vpos_strobe;            /**< source positive, 0..3                 */
    uint8_t vee_strobe;             /**< gate low, 0..3                        */
    uint8_t vneg_strobe;            /**< source negative, 0..3                 */
    uint8_t delay_per_strobe_ms;    /**< 3, 6, 9 or 12                         */
} epd_power_seq_t;

/** A complete panel description. */
typedef struct {
    const char *name;           /**< for logs                                  */

    uint16_t width;             /**< pixels; must be a multiple of 8           */
    uint16_t height;            /**< pixels (gate rows)                        */
    uint8_t  bus_width;         /**< source bus width: 8 or 16                 */
    uint32_t pclk_hz;           /**< source shift clock; typical EPD 4-20 MHz  */

    /**
     * Dummy bytes clocked out after each row's pixel data.
     *
     * The 2bpp packing puts 4 pixels in a byte on any bus width, so this is a
     * pixel count divided by 4 and is bus-width independent: 16 bytes is 64
     * pixels of extra shift either way.
     *
     * Panels differ in how many source-driver stages sit beyond the visible
     * area, so if the image comes out shifted horizontally by a constant
     * amount, this is the first thing to change.  Was hardcoded at 16.
     */
    uint8_t line_padding_bytes;

    /**
     * VCOM voltage as a positive magnitude in millivolts.
     *
     * This is a property of the individual panel and is printed on its FPC
     * ribbon cable - a label of "-1.60V" means 1600.  It is not portable even
     * between two panels of the same model, so check the ribbon rather than
     * copying a definition.  Getting it wrong is not merely a contrast
     * problem: several artefacts scale with VCOM and stay invisible when it is
     * mis-set near zero.
     */
    uint16_t vcom_mv;

    /**
     * Orientation, EPD_PANEL_FLAG_* combined with |.
     *
     * This corrects how the panel's own source and gate drivers map onto
     * framebuffer coordinates, which is why it belongs to the panel rather
     * than to the application.  Use MIRROR_X|MIRROR_Y for 180 degrees.
     */
    uint8_t  flags;

    bool temp_compensation;     /**< scale frame counts by panel temperature   */

    epd_power_seq_t    power;
    epd_drive_codes_t  codes;
    epd_ac_timing_t    timing;
    epd_waveform_def_t wf;
} epd_panel_def_t;

/**
 * @brief  Validate a panel definition.
 *
 * Checks the invariants the driver relies on: an 8-pixel-aligned width (the
 * 2bpp source packing puts 4 pixels in a byte, and the DU row builder works in
 * 8-pixel groups), a supported bus width, and phase counts within range.
 *
 * @return ESP_OK when the definition is usable.
 */
esp_err_t epd_panel_def_validate(const epd_panel_def_t *def);

#ifdef __cplusplus
}
#endif

#endif /* EPD_PANEL_DEF_H */
