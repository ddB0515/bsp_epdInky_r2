#include "epd_panels.h"
#include "esp_check.h"

static const char *TAG = "epd_panels";

/*******************************************************************************
 * 1872x1404 16-bit parallel E Ink panel
 *
 * Every number below was measured or tuned on this panel.  Notes on the ones
 * that are not obvious:
 *
 * timing      Empirically validated against this panel with a TPS65185.  The
 *             frame-start sequence is timing critical; see frame_gate_start().
 *
 * row_period  Left at 0 (free-run).  Pacing was added so the drive dwell could
 *             be pinned, but this panel proved insensitive to it: dwell was cut
 *             ~30 % across two optimisation steps with no visible change,
 *             because tone here comes from how many phases drive a pixel rather
 *             than from how long each row is held.
 *
 * codes       Hardware-confirmed: 0b01 = VNEG darkens, 0b10 = VPOS lightens.
 *
 * level_energy / phase_cut / bayer
 *             Tuned against the greyscale bars and palette screens.  The panel
 *             resolves roughly 4 reliable temporal tones, so 16 levels are
 *             synthesised from 4 phases plus the 4x4 dither.
 *
 * init_group_frames
 *             4 gives 16 INIT frames.  Measured: 2 (8 frames) leaves the
 *             previous image ghosting through the background, so this is close
 *             to the panel's floor.  Since refresh time is proportional to
 *             drive energy, INIT is drive-limited rather than overhead-limited
 *             and cannot be shortened in software.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed103tc2 = {
    .name      = "ED103TC2",

    .width     = 1872,
    .height    = 1404,
    .bus_width = 16,
    .pclk_hz   = 20000000,

    /*
     * Read from this panel's FPC ribbon: "-1.25V".  Check yours rather than
     * trusting this number - VCOM varies between individual panels and is not
     * portable even between two of the same model.
     *
     * Kept here rather than overridden by the application: an app-side
     * override is easy to lose in an edit and fails silently, because a wrong
     * VCOM degrades the image instead of reporting an error.
     */
    .vcom_mv   = 1250,

    /* This panel's source drivers run right-to-left relative to the framebuffer. */
    .flags     = EPD_PANEL_FLAG_MIRROR_Y,

    .temp_compensation = true,

    /* Was hardcoded as ROW_TX_BYTES = row_bytes + 16. */
    .line_padding_bytes = 16,

    /*
     * VEE -> VNEG -> VPOS -> VDDH, 9 ms apart.  These are exactly the values
     * TPS65185.c used to program for every panel; they now travel with the
     * panel that was validated against them.
     */
    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,     /* t1 = 1 line, as validated */
        .ckv_low_us       = 0,     /* validated with back-to-back CKV edges */
        .le_pulse_us      = 0,     /* validated with back-to-back LE edges */

        .interframe_us    = 230,
        .row_period_us    = 0,     /* free-run; see note above */
        .ckv_flush_us     = 3,     /* keeps CKV inside its 200 kHz maximum */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

esp_err_t epd_panel_def_validate(const epd_panel_def_t *def)
{
    ESP_RETURN_ON_FALSE(def, ESP_ERR_INVALID_ARG, TAG, "NULL panel definition");

    ESP_RETURN_ON_FALSE(def->width && def->height,
                        ESP_ERR_INVALID_ARG, TAG, "zero geometry");

    /*
     * 2bpp source packing puts 4 pixels in a byte, but build_row_du() walks the
     * row in 8-pixel groups and stores 16 bits at a time, so it needs the
     * stronger alignment.  A width that is a multiple of 4 but not 8 would read
     * one byte past the end of the last source row and write one byte past
     * row_bytes into the line padding.
     */
    ESP_RETURN_ON_FALSE(def->width % 8 == 0, ESP_ERR_INVALID_ARG, TAG,
                        "width %u must be a multiple of 8", def->width);

    ESP_RETURN_ON_FALSE(def->bus_width == 8 || def->bus_width == 16,
                        ESP_ERR_INVALID_ARG, TAG,
                        "bus_width %u must be 8 or 16", def->bus_width);

    ESP_RETURN_ON_FALSE(def->pclk_hz > 0, ESP_ERR_INVALID_ARG, TAG,
                        "pclk_hz must be non-zero");

    ESP_RETURN_ON_FALSE(def->wf.gc16_phases > 0 &&
                        def->wf.gc16_phases <= EPD_GC16_MAX_PHASES,
                        ESP_ERR_INVALID_ARG, TAG,
                        "gc16_phases %u out of range 1..%d",
                        def->wf.gc16_phases, EPD_GC16_MAX_PHASES);

    for (uint8_t i = 0; i < def->wf.gc16_phases; i++) {
        ESP_RETURN_ON_FALSE(def->wf.gc16_phase_order[i] < def->wf.gc16_phases,
                            ESP_ERR_INVALID_ARG, TAG,
                            "gc16_phase_order[%u] = %u is out of range",
                            i, def->wf.gc16_phase_order[i]);
    }

    /*
     * A pixel drives in a phase when energy + bayer >= cut.  If a cut does not
     * exceed the largest Bayer value then some cells of that phase drive even
     * at zero energy, which puts drive into pixels that should stay white.
     */
    uint16_t bayer_max = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            if (def->wf.bayer[y][x] > bayer_max) bayer_max = def->wf.bayer[y][x];
        }
    }
    for (uint8_t i = 0; i < def->wf.gc16_phases; i++) {
        ESP_RETURN_ON_FALSE(def->wf.gc16_phase_cut[i] > bayer_max,
                            ESP_ERR_INVALID_ARG, TAG,
                            "gc16_phase_cut[%u] = %u must exceed the largest "
                            "bayer value (%u) or that phase drives at zero energy",
                            i, def->wf.gc16_phase_cut[i], bayer_max);
    }

    ESP_RETURN_ON_FALSE(def->wf.init_group_frames > 0 && def->wf.du_frames > 0,
                        ESP_ERR_INVALID_ARG, TAG, "frame counts must be non-zero");

    /* Zero would emit no frame sync at all and leave the gate driver unpositioned. */
    ESP_RETURN_ON_FALSE(def->timing.spv_sync_lines > 0, ESP_ERR_INVALID_ARG, TAG,
                        "spv_sync_lines must be at least 1");

    /*
     * The four rails must occupy four distinct strobes; two rails sharing one
     * would bring them up together and defeat the ordering the panel asked for.
     */
    const uint8_t strobes[4] = {
        def->power.vddh_strobe, def->power.vpos_strobe,
        def->power.vee_strobe,  def->power.vneg_strobe,
    };
    uint8_t seen = 0;
    for (int i = 0; i < 4; i++) {
        ESP_RETURN_ON_FALSE(strobes[i] < 4, ESP_ERR_INVALID_ARG, TAG,
                            "power strobe %d = %u is out of range 0..3",
                            i, strobes[i]);
        seen |= (uint8_t)(1u << strobes[i]);
    }
    ESP_RETURN_ON_FALSE(seen == 0x0Fu, ESP_ERR_INVALID_ARG, TAG,
                        "power strobes must be a permutation of 0..3");

    /*
     * No transfer-size alignment check.
     *
     * An earlier version of this validator required the row transfer to be a
     * multiple of 4, inferred from the fact that the panels tried so far all
     * happened to be.  That was wrong, and it would have rejected a valid
     * 600-pixel-wide panel (150 + 16 = 166 bytes).
     *
     * The i80 driver checks color_size against the GDMA channel's
     * int_mem_alignment, which starts at 1 and is only raised for RX channels
     * or when CONFIG_GDMA_ENABLE_WEIGHTED_ARBITRATION is set.  The LCD channel
     * is TX and that option is not enabled here, so the alignment is 1 and any
     * size is acceptable.  The cross-check is that epd_i80_bus.c requests a
     * 64-byte DMA burst: were the burst size being folded into the alignment,
     * the 1872x1404 panel's 484-byte rows would already fail, and they do not.
     *
     * width % 8 == 0 above is the constraint that actually matters, and it
     * also guarantees row_bytes is even for build_row_du's 16-bit stores.
     */

    return ESP_OK;
}

/*******************************************************************************
 * E Ink ED097TC2 — 9.7", 1200 (H) x 825 (V), 8-bit source bus
 *
 * UNVALIDATED.  Geometry and AC timing come from the panel's datasheet, but
 * nothing here has been confirmed against real glass, and the waveform block is
 * inherited from the 1872x1404 panel rather than tuned for this ink.  Expect to
 * work through the items flagged below before the image looks right.
 *
 * From the datasheet's AC characteristics table (VDD 3.0-3.6 V):
 *
 *   fckv   max 200 kHz  -> CKV period >= 5 us.  ckv_flush_us = 3 gives a 6 us
 *                         period (167 kHz), inside the limit.
 *   twL    min 0.5 us   -> minimum CKV LOW pulse.  This is why ckv_low_us
 *                         exists: the driver otherwise drives CKV low and high
 *                         back to back, giving roughly 100 ns.  Set to 1 us for
 *                         2x margin.  It applies per row, so it adds 1 us to
 *                         the row period.
 *   tSU    min 100 ns   -> SPV setup before the CKV edge.
 *   tH     min 100 ns   -> SPV hold after it.  The microsecond-scale values
 *                         below clear both by a wide margin.
 *   tcy    min 50 ns    -> CL cycle, so pclk_hz <= 20 MHz.  Set to 16 MHz to
 *                         leave margin on an unvalidated panel; raise it once
 *                         the panel is known good.
 *   tLEw   min 40 ns    -> LE high pulse.  Currently two GPIO writes, safely
 *                         over 40 ns, but this is worth rechecking if the GPIO
 *                         path is ever made faster.
 *   D0..D7              -> eight data lines, hence bus_width = 8.  Confirm the
 *                         board actually wires D0-D7 to this panel's connector.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed097tc2 = {
    .name      = "ED097TC2",

    .width     = 1200,
    .height    = 825,
    .bus_width = 8,            /* datasheet lists D0..D7 only */
    .pclk_hz   = 16000000,     /* datasheet max is 20 MHz (tcy >= 50 ns) */

    /*
     * MUST BE CHECKED against the value printed on this panel's own FPC
     * ribbon - it is not portable even between two ED097TC2 units.  2000 is a
     * placeholder typical of the family, not a datasheet figure.  Note that a
     * mis-set VCOM near zero HIDES artefacts that scale with it, so a picture
     * that looks acceptable is not evidence the value is right.
     */
    .vcom_mv   = 1620,  /* placeholder; read from the panel's FPC ribbon */

    /*
     * Assumes this panel's source drivers run right-to-left relative to the
     * framebuffer, matching the other panel on this board.  If text renders
     * mirrored, change to EPD_PANEL_FLAG_NONE.
     */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    /*
     * Same as the other panel.  Because the 2bpp packing is 4 pixels per byte
     * on any bus width, 16 bytes is 64 pixels of extra shift here too - the
     * 8-bit bus does not change this.  Suspect it first if the image appears
     * shifted horizontally by a constant amount.
     */
    .line_padding_bytes = 16,

    /*
     * Checked against the datasheet's power-on sequence, which specifies two
     * independent chains:
     *
     *     1. VSS -> VDD -> VNEG -> VPOS    (source driver)
     *     2. VEE -> VGG                    (gate driver)
     *
     * VEE -> VNEG -> VPOS -> VDDH satisfies both: VNEG precedes VPOS, and VEE
     * precedes VGG (the datasheet's VGG is the PMIC's VDDH).  The datasheet
     * does not constrain the two chains relative to each other.
     *
     * Its inter-rail minimums are T1 = 100 us and T3 = 1000 us, so the 9 ms
     * step clears them by 9x to 90x.
     */
    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    /*
     * Standard E Ink source encoding, same as the 1872x1404 panel.  Not
     * confirmed for this part: if the image comes out inverted, darken and
     * lighten are swapped.
     */
    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        /*
         * Generous relative to the datasheet minimums, which are in the
         * hundreds of nanoseconds.  Too slow only costs frame-start time and
         * is safe; too fast misplaces the gate and shifts the whole frame.
         */
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,     /* t1 = 1 line, as validated */
        .ckv_low_us       = 1,     /* twL min 0.5 us, 2x margin */
        .le_pulse_us      = 0,     /* validated with back-to-back LE edges */

        .interframe_us    = 230,
        .row_period_us    = 0,     /* free-run; well above the 5 us CKV period */
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under the 200 kHz max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /*
     * Tone curve, gamma corrected.
     *
     * This started as a linear ramp from 64 at black to 0 at white, chosen as
     * an unbiased starting point for measuring the panel.  That is fine for
     * reading the bars screen, but it renders photographs noticeably dark,
     * because perceived lightness is not linear in drive frames: a linear
     * energy ramp spends too much drive on the midtones.  Measured, it averages
     * 2.00 drive frames per level against 1.34 for the curve that looks correct
     * on ED103TC2.
     *
     * These values are energy[i] = 64 * ((15 - i) / 15) ^ 1.8, which sits
     * between the linear ramp and ED103TC2's curve (itself very close to a
     * gamma of 2.1).
     *
     * TUNING, in the order worth reaching for:
     *
     *   Too dark or too light overall
     *     Re-generate this curve with a different gamma.  Higher gamma is
     *     lighter: 1.4 gives a mean of 1.69 drive frames, 1.8 gives 1.47, and
     *     2.2 gives 1.30.  This is the knob that actually moves brightness.
     *
     *   Uniformly too dark, but the tone spacing looks right
     *     Lower the curve's PEAK energy, keeping the same gamma - for example
     *     56 instead of 64, as ED115OC1 does.
     *
     *     Do NOT raise gc16_phase_cut for this.  Bayer only spans 0..15, so a
     *     pixel drives at all only when energy + 15 >= cut[0]; pushing cut[0]
     *     from 16 to 24 makes every level with energy under 9 render pure
     *     white, collapsing the light end.  The thresholds are matched to the
     *     Bayer range and are best left at 16/32/48/64.
     *
     *   Two adjacent levels look identical on the bars screen
     *     Spread their energy values further apart by hand.
     *
     *   Banding or a visible dither texture
     *     That is the bayer matrix's department.  Note it is a permutation,
     *     so its mean is fixed at 7.5: reordering it changes WHERE pixels
     *     drive, never how many, and so cannot be used to change brightness.
     */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        /* energy[i] = round(64 * ((15 - i) / 15) ^ 1.8) */
        .gc16_level_energy = {
            64, 57, 49, 43, 37, 31, 26, 21,
            16, 12,  9,  6,  4,  2,  0,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED113TC1 — 2400 (H) x 1034 (V), 16-bit source bus
 *
 * Deliberately a near-copy of the ED103TC2 definition, because its purpose is
 * diagnostic: it is a SECOND 16-bit panel, so driving it isolates whether a
 * blank ED097TC2 is caused by the 8-bit path or by something common to all
 * panels.  Everything except geometry and the two timing fields noted below is
 * identical to the known-good panel, so a failure here would point at the
 * driver rather than at bus width.
 *
 * Note that main/owl.h, deleted earlier as unused, was 2400x1034 and its
 * OWL_IMAGE_SIZE of 1240800 bytes is exactly this panel's framebuffer, so this
 * hardware was very likely the project's original target.
 *
 * From the datasheet's AC characteristics (VDD 3.0-3.6 V), and how it differs
 * from what the driver already does:
 *
 *   tcy    min 16.7 ns  -> the source clock may go to roughly 60 MHz, far
 *                          faster than the ED097TC2's 50 ns floor.  Left at
 *                          20 MHz to match the known-good panel; this is a
 *                          diagnostic, not a performance run.
 *   twL    min 0.5 us   -> minimum CKV LOW pulse, hence ckv_low_us = 1 for 2x
 *                          margin.  ED103TC2 runs with 0 and works, so if this
 *                          panel misbehaves, 0 is a one-word thing to try.
 *   twH    min 0.5 us   -> minimum CKV HIGH pulse.  Satisfied by construction:
 *                          CKV is held high across each row's DMA, which is
 *                          about 15 us at 20 MHz, and across the multi-
 *                          microsecond delays in the frame start sequence.
 *   tLEoff min 200 ns   -> delay from LE falling to the next edge.  Satisfied
 *                          by the 1 us ckv_low_us gap that now follows the LE
 *                          pulse in gate_advance().
 *   tSU/tH min 100 ns   -> SPV setup and hold around the CKV edge, cleared by
 *                          the microsecond-scale gate timings.
 *   tout   max 12 us    -> source settling to +/-30 mV at 200 pF.  Comfortably
 *                          inside the row period, which is about 60 us.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed113tc1 = {
    .name      = "ED113TC1",

    .width     = 2400,
    .height    = 1034,
    .bus_width = 16,
    .pclk_hz   = 20000000,     /* datasheet allows ~60 MHz; matching ED103TC2 */

    .vcom_mv   = 1200,         /* panel is marked -1.2V */

    /* Assumed to match ED103TC2; if text renders mirrored, use FLAG_NONE. */
    .flags     = EPD_PANEL_FLAG_MIRROR_X,

    .temp_compensation = true,

    /* Same as ED103TC2: 16 bytes = 64 pixels of extra shift. */
    .line_padding_bytes = 16,

    /* Same ordering as ED103TC2. */
    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,     /* t1 = 1 line, as validated */
        .ckv_low_us       = 1,     /* twL min 0.5 us; ED103TC2 uses 0 */
        .le_pulse_us      = 0,     /* validated with back-to-back LE edges */

        .interframe_us    = 230,
        .row_period_us    = 0,
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under the 200 kHz max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /*
     * Copied verbatim from ED103TC2 rather than neutralised, because this
     * definition exists to answer "does a 16-bit panel light up at all".
     * Reusing the known-good curve maximises the chance of a recognisable
     * image; it is not a claim that it is correct for this ink.  Re-tune
     * against the bars screen once the panel is confirmed working.
     */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED060SCP — 6", 600 (H) x 800 (V), 8-bit source bus
 *
 * The second 8-bit panel, and deliberately a near-copy of the known-good
 * ED103TC2 apart from geometry, bus width and the two timings noted below.
 * ED113TC1 (16-bit, different geometry) drives correctly while ED097TC2
 * (8-bit) shows nothing, so this exists to tell those two apart: if this panel
 * also stays blank, the 8-bit path or its wiring is implicated rather than the
 * individual panel.
 *
 * The waveform block is copied verbatim from ED103TC2 rather than neutralised,
 * for the same reason as ED113TC1 - a known-good tone curve maximises the
 * chance of a recognisable image on a "does it light up at all" test.  It is
 * not a claim that the curve suits this ink.
 *
 * From the datasheet's AC characteristics (VDD 3.0-3.6 V):
 *
 *   fckv   max 200 kHz  -> ckv_flush_us 3 gives a 6 us period (167 kHz).
 *   twL    min 0.5 us   -> ckv_low_us = 1 for 2x margin.  No twH is specified
 *                          for this panel, unlike ED113TC1.
 *   tcy    min 50 ns    -> source clock <= 20 MHz, the same ceiling as
 *                          ED097TC2 and matching ED103TC2's 20 MHz.
 *   tLEoff min 200 ns   -> delay after LE falls, satisfied by the 1 us
 *                          ckv_low_us gap that follows the LE pulse.
 *   tSU/tH min 100 ns   -> cleared by the microsecond-scale gate timings.
 *   tout   max 12 us    -> source settling at 200 pF, inside the row period.
 *
 * Geometry note: this panel was first entered as 600 wide, giving a 166-byte
 * row transfer, and that is what prompted the transfer-size alignment check
 * discussed in epd_panel_def_validate() - a check which turned out to be
 * unfounded and was removed.  At the correct 800 pixels the row is 216 bytes;
 * the panels that genuinely have a row length not divisible by 4 are ED115OC1
 * (706) and ED133UT2 (566), and both work.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed060scp = {
    .name      = "ED060SCP",

    .width     = 800,
    .height    = 600,
    .bus_width = 8,            /* datasheet lists D0..D7 */
    .pclk_hz   = 20000000,     /* tcy min 50 ns -> 20 MHz ceiling */

    .vcom_mv   = 1600,         /* panel is marked -1.6V */

    /* Assumed to match the other panels; if text renders mirrored, FLAG_NONE. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    /* 16 bytes = 64 pixels of shift, as on every other panel here. */
    .line_padding_bytes = 16,

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,     /* t1 = 1 line, as validated */
        .ckv_low_us       = 1,     /* twL min 0.5 us */
        .le_pulse_us      = 0,     /* validated with back-to-back LE edges */

        .interframe_us    = 230,
        .row_period_us    = 0,
        .ckv_flush_us     = 3,

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /* Copied from ED103TC2; re-tune once the panel is confirmed working. */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED052TC4 — 720 (H) x 1280 (V), 8-bit source bus
 *
 * From the datasheet's AC characteristics, and how it differs from the panels
 * already supported here:
 *
 *   tLEw   min 300 ns  -> THE significant difference.  Every other panel here
 *                         asks for 40 ns, which the driver's two back-to-back
 *                         GPIO writes happen to satisfy.  300 ns they do not,
 *                         so le_pulse_us exists for this panel: without it the
 *                         row data is never reliably latched onto the source
 *                         outputs.  1 us gives over 3x margin and tLEw has no
 *                         maximum.
 *   twL    min 0.5 us  -> ckv_low_us = 1, as on the other recent panels.
 *   twH    min 0.5 us  -> satisfied by construction; CKV is held high across
 *                         each row's DMA and the frame-start delays.
 *   tcy    min 16.67 ns, typ 50 ns
 *                      -> the source clock could reach ~60 MHz.  Held at
 *                         20 MHz, matching the datasheet's typical and the
 *                         other panels, for first bring-up.
 *   tLEdly min 3.5*tcy -> delay before LE rises, 175 ns at the typical tcy.
 *                         Satisfied with room to spare: LE is pulsed only after
 *                         the DMA completion interrupt has woken the refresh
 *                         task, which is microseconds.
 *   tLEoff min 200 ns  -> delay after LE falls, covered by the 1 us ckv_low_us
 *                         gap that follows the LE pulse in gate_advance().
 *   tout   max 20 us   -> source settling at 200 pF, longer than the 12 us of
 *                         the other panels but still inside the row period.
 *
 * VCOM is -2.57 V, roughly double every other panel here.  That is within the
 * TPS65185's range (max -5.11 V in 10 mV steps) so it programs exactly, but it
 * does mean this panel's drive conditions differ substantially from the ones
 * the shared tone curve was measured on.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed052tc4 = {
    .name      = "ED052TC4",

    .width     = 1280,
    .height    = 720,
    .bus_width = 8,
    .pclk_hz   = 10000000,     /* datasheet typ tcy 50 ns; min allows ~60 MHz */

    .vcom_mv   = 2570,         /* panel is marked -2.57V */

    /*
     * The two working 8-bit panels on this board both need FLAG_NONE while the
     * 16-bit ones need MIRROR_X, which follows from the two bus widths taking
     * their first byte from opposite halves of the connector.  If text renders
     * mirrored, this is the field to change.
     */
    .flags     = EPD_PANEL_FLAG_MIRROR_X,

    .temp_compensation = true,

    .line_padding_bytes = 16,

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,     /* t1 = 1 line, as validated */
        .ckv_low_us       = 1,     /* twL min 0.5 us */
        .le_pulse_us      = 1,     /* tLEw min 300 ns - required by this panel */

        .interframe_us    = 230,
        .row_period_us    = 0,
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under the 200 kHz max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /*
     * Borrowed from ED103TC2 for first bring-up, on the reasoning that a curve
     * known to produce a recognisable image is the best starting point for
     * answering "does this panel drive at all".  It is a guess, not a
     * measurement, and this panel's VCOM is about double the one it was tuned
     * against, so expect the greys to be wrong.  Re-tune against the bars
     * screen once the panel is confirmed working.
     */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED115OC1 — 2760 (H) x 2070 (V), 16-bit source bus
 *
 * The largest panel here: its framebuffer is 2.86 MB and a full INIT clocks
 * 2070 rows x 16 frames, so expect refreshes to take appreciably longer than
 * on the smaller panels.  Refresh time is HEIGHT x frames x row period and is
 * proportional to drive energy, so there is no way to shorten it in software
 * without giving the pixels less drive.
 *
 * From the datasheet's AC characteristics (VDD 3.0-3.6 V):
 *
 *   t1     min 2 LINES -> THE significant difference.  Frame Sync Length: the
 *                         STV pulse must span at least two CKV lines.  Every
 *                         other panel here is satisfied by the single pulse the
 *                         driver has always emitted, so spv_sync_lines exists
 *                         for this one.  Too short a frame sync leaves the gate
 *                         driver mis-positioned, which shows up as the image
 *                         being offset vertically or absent altogether.
 *   fckv   max 200 kHz  -> ckv_flush_us 3 gives a 6 us period (167 kHz).
 *   twL    min 0.5 us   -> ckv_low_us = 1.
 *   twH    min 0.5 us   -> satisfied by construction; CKV is held high across
 *                          each row's DMA and the frame-start delays.
 *   tcy    min 16.7 ns, typ 20 ns
 *                       -> the source clock could reach ~60 MHz.  Held at
 *                          20 MHz to match the other working panels; worth
 *                          raising here once validated, since this panel's
 *                          690-byte rows make DMA a larger share of the row
 *                          period than on the smaller panels.
 *   tSU/tH min 100 ns   -> STV setup and hold, cleared by the microsecond-scale
 *                          gate timings.
 *   tLEw   min 40 ns    -> satisfied by the two back-to-back GPIO writes, so
 *                          le_pulse_us stays 0 (unlike ED052TC4's 300 ns).
 *   tLEoff min 200 ns   -> covered by the 1 us ckv_low_us gap after the LE
 *                          pulse in gate_advance().
 *   tout   max 12 us    -> source settling at 200 pF, inside the row period.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed115oc1 = {
    .name      = "ED115OC1",

    .width     = 2760,
    .height    = 2070,
    .bus_width = 16,
    .pclk_hz   = 25000000,     /* datasheet allows ~60 MHz; matching the others */

    .vcom_mv   = 1340,         /* panel is marked -1.34V */

    /* 16-bit panels on this board take MIRROR_X; see the wiring note in
     * epd_display.c for why the two bus widths differ here. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    .line_padding_bytes = 16,

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 2,     /* t1 min 2 lines - required by this panel */
        .ckv_low_us       = 1,     /* twL min 0.5 us */
        .le_pulse_us      = 0,     /* tLEw is only 40 ns here */

        .interframe_us    = 230,
        .row_period_us    = 0,
        .ckv_flush_us     = 3,

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /*
     * Tone curve: LINEAR, peak 40.
     *
     * The important property of this waveform model is that, with the 4x4
     * Bayer dither and thresholds at 16/32/48/64, average drive is EXACTLY
     * energy / 16 frames.  The dither linearises it: energy 16 gives 1.00
     * frames, energy 40 gives 2.50, and so on across the whole range.
     *
     * That means a gamma-shaped energy table produces gamma-shaped DRIVE, and
     * on this panel that crushed both ends.  A gamma of 2.8 put levels 11-15
     * within 0.06 frames of each other - rendering 13, 14 and 15 as the same
     * white - while levels 0-3 stayed bunched in the top 1.6 frames and read
     * as one black.
     *
     * So the energy table is kept linear, which spaces all sixteen levels
     * evenly at about 0.17 frames apart, and the peak sets how hard level 0 is
     * driven.  Separation and overall darkness become independent knobs:
     *
     *   PEAK  the dark end.  Level 0 gets peak/16 frames, here 2.50.  Lower it
     *         (36, 32) if the darkest levels still merge into one black; raise
     *         it (44, 48) if level 0 is not properly black.  Every other level
     *         scales with it, so separation is preserved either way.
     *
     *   SHAPE do NOT reintroduce gamma here.  Perceptual brightness belongs in
     *         the image, where convert_image.py has 8 bits to work with:
     *         --gamma above 1 darkens, below 1 lightens.  Bending this table
     *         instead spends the panel's limited 4-phase range on a correction
     *         that costs level separation.
     *
     *   Thresholds stay at 16/32/48/64: they are matched to the Bayer range,
     *   and raising them makes low-energy levels stop driving entirely.
     */
    .wf = {
        /*
         * One phase.  Level 0 reaches full coverage exactly at the cut, so
         * nothing needs a second frame: energies of 65 and 92 were previously
         * indistinguishable from 64 on this panel, which is what says drive
         * beyond full coverage adds no further darkening.  Halves GC16 time.
         */
        .gc16_phases     = 1,
        .gc16_phase_cut  = { 64 },
        .gc16_phase_order = { 0 },

        .gc16_level_energy = {
            /*
             * Measured, not modelled.  Photograph the 4x4 palette, correct for
             * the lighting gradient using the panel's own white margins, read
             * the reflectance of all 16 patches, then invert that curve so the
             * levels land at even CIE L* intervals.
             *
             * The thing that makes this panel awkward is severe dot gain: a
             * driven pixel darkens its neighbours, so reflectance falls far
             * faster than the dot coverage alone would suggest.
             *
             *     coverage    ideal r    measured r
             *        25%       0.77         0.62
             *        50%       0.54         0.25
             *        75%       0.31         0.13
             *
             * At half coverage the panel is already 82% of the way to black.
             * That is why the greys have to be squeezed into the bottom third
             * of the coverage range and why the midtones look far too dark on
             * any curve derived from coverage alone - the previous attempt put
             * level 8 at L* 47.9 when even spacing wants 69.3.
             *
             * It also compresses the dark end: levels 0-3 spanned just 4.7 L*
             * and read as one flat black.  Spreading them to 13.4 L* takes a
             * much steeper drop in energy (64, 60, 48, 39) than looks sensible
             * on paper.
             *
             * Resulting step sizes are 3.2-4.8 L* against an ideal of 4.35.
             * The residue is the 1/64 quantisation of the 8x8 dither, which is
             * also why the top two entries stay close together.
             */
             64, 60, 48, 39, 36, 33, 29, 26,
             23, 20, 18, 16, 14, 12,  8,  0,
        },

        .bayer = {
            /* full 8x8 ordered dither, 0..63 -> drive resolution of 1/64 frame */
            {  0, 32,  8, 40,  2, 34, 10, 42 },
            { 48, 16, 56, 24, 50, 18, 58, 26 },
            { 12, 44,  4, 36, 14, 46,  6, 38 },
            { 60, 28, 52, 20, 62, 30, 54, 22 },
            {  3, 35, 11, 43,  1, 33,  9, 41 },
            { 51, 19, 59, 27, 49, 17, 57, 25 },
            { 15, 47,  7, 39, 13, 45,  5, 37 },
            { 63, 31, 55, 23, 61, 29, 53, 21 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED133UT2 — 2200 (H) x 1650 (V), 16-bit source bus
 *
 * 13.3" A4-format panel, the one used in the Sony DPT-RP1.  Largest framebuffer
 * after ED115OC1 at 1.73 MB (2200/2 x 1650), so it needs PSRAM.
 *
 * From the datasheet's AC characteristics (VDD 2.75-3.6 V):
 *
 *   tcy    min 16.7 ns  -> source clock may reach ~60 MHz.  Left at 20 MHz to
 *                          match the panels already validated on this board;
 *                          raise it once the panel is confirmed working.
 *   twL    min 0.5 us   -> minimum CKV LOW pulse, hence ckv_low_us = 1.
 *   twH    min 0.5 us   -> minimum CKV HIGH pulse.  Satisfied by construction:
 *                          CKV is held high across each row's DMA.
 *   tLEw   min 40 ns    -> LE high pulse.  Back-to-back GPIO writes clear this,
 *                          so le_pulse_us = 0, as on every panel but ED052TC4.
 *   tLEoff min 200 ns   -> satisfied by the 1 us ckv_low_us gap after LE.
 *   tSU/tH min 100 ns   -> SPV setup and hold, cleared by the microsecond-scale
 *                          gate timings.
 *   tout   max 12 us    -> source settling, well inside the ~60 us row period.
 *   fckv   max 200 kHz  -> ckv_flush_us = 3 gives a 6 us period = 167 kHz.
 *   t1     = 1 line     -> spv_sync_lines = 1 (ED115OC1 is the odd one at 2).
 *
 * NOTE ON BUS WIDTH.  The AC table names only "D0 .. D7", which on its own
 * would read as an 8-bit source bus; the panel is specified here as 16-bit per
 * the hardware it came from.  These are wired to different GPIO banks on this
 * board, so if the panel comes up blank this is the first thing to change -
 * that exact confusion is what kept ED097TC2 and ED060SCP dark.  A wrong
 * bus_width leaves the source lines undriven rather than producing a garbled
 * image, so "blank" is its signature.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed133ut2 = {
    .name      = "ED133UT2",

    .width     = 2200,
    .height    = 1650,
    .bus_width = 16,
    .pclk_hz   = 20000000,     /* datasheet allows ~60 MHz */

    .vcom_mv   = 1890,         /* panel is marked -1.89V */

    /* Every 16-bit panel on this board has needed MIRROR_X; if text renders
     * mirrored, use FLAG_NONE. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    .line_padding_bytes = 16,  /* 64 pixels of extra shift, as on the others */

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,     /* t1 = 1 line */
        .ckv_low_us       = 1,     /* twL min 0.5 us */
        .le_pulse_us      = 0,     /* tLEw min 40 ns */

        .interframe_us    = 230,
        .row_period_us    = 0,
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under fckv max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /*
     * Starting point is the ED103TC2 curve, which is the longest-validated one
     * in this file, on the classic 4x4-tiled dither with cuts 16/32/48/64.
     * Deliberately NOT the ED115OC1 curve: that one was measured against that
     * panel's very severe dot gain and is specific to it.
     *
     * Expect to re-tune against the 4x4 palette screen.  The procedure that
     * worked for ED115OC1: photograph the palette, correct for the lighting
     * gradient using the panel's own white margins, read the 16 patch
     * reflectances, then invert that curve for even CIE L* spacing.  Reasoning
     * about the curve without measuring it repeatedly gave wrong answers.
     */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED070KH1 — 1680 (H) x 1264 (V), 7"
 *
 * NO DATASHEET AVAILABLE.  The AC timing below is copied from ED103TC2, which
 * is the longest-validated definition in this file, on the basis that every
 * panel brought up so far has needed the same gate sequence and differed only
 * in three fields (ckv_low_us, le_pulse_us, spv_sync_lines).  ED133UT2 worked
 * on the first attempt with exactly these numbers.
 *
 * The one deliberate deviation is ckv_low_us = 1 rather than ED103TC2's 0.
 * Without a twL figure to check against, the driver's back-to-back GPIO writes
 * give a CKV LOW pulse of only ~100 ns, which is below the 0.5 us minimum that
 * every panel that DOES document twL specifies.  ED103TC2 tolerates it; there
 * is no reason to assume this one does.  Set it to 0 if you want to match
 * ED103TC2 exactly - it costs about 1 us per row.
 *
 * vcom_mv is read from the panel itself ("-1.40V")
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed070kh1 = {
    .name      = "ED070KH1",

    .width     = 1264,
    .height    = 1680,
    .bus_width = 16,
    .pclk_hz   = 20000000,

    .vcom_mv   = 1400,         /* panel is marked -1.40V */

    /* Every 16-bit panel on this board has needed MIRROR_X; if text renders
     * mirrored, use FLAG_NONE. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    .line_padding_bytes = 16,

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 18,
        .spv_sync_lines   = 1,
        .ckv_low_us       = 1,     /* margin for an undocumented twL */
        .le_pulse_us      = 0,

        .interframe_us    = 230,
        .row_period_us    = 0,
        .ckv_flush_us     = 3,
        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /* ED103TC2's curve, on the classic 4x4-tiled dither.  Re-tune against the
     * palette screen once the panel is confirmed working - and only after VCOM
     * is correct, since a wrong VCOM looks exactly like a wrong curve. */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED067KC1 — 1800 px/line x 900 lines/frame, 8-bit source bus, 6.7" bar panel
 *
 * Two different, both-correct-in-their-own-terms numbers for this panel, which
 * cost two rounds of bring-up to untangle:
 *
 *   Mechanical spec:        900(H) x 1800(V)   - the panel's physical mounting
 *                                                orientation (tall and narrow).
 *   Timing Parameters table: 1800 x 900        - the ELECTRICAL scan direction
 *                                                (LDL = 450 SDCK x 4 px/SDCK =
 *                                                1800 px/line; FDL = 900
 *                                                lines/frame).
 *
 * width/height in this struct are the electrical numbers - the source bus
 * shifts 1800 pixels per row and there are 900 gate rows total - not the
 * mechanical H/V labelling, which describes how the glass is meant to be
 * mounted, not which direction it scans in. Both happen to be clean multiples
 * of 8 (1800/8 = 225), so no width padding is needed here, unlike the
 * width=900 dead end an earlier reading of this panel went through.
 *
 * From the datasheet's Timing Parameters table (the actual frame/gate timing
 * diagram - distinct from, and more specific than, the AC characteristics
 * table used for everything below it):
 *
 *   FSL (Frame Start Length) = 1 line  -> spv_sync_lines = 1, like every panel
 *                          here except ED115OC1. An earlier guess of 2 (on
 *                          the assumption this panel might be ED115OC1-like
 *                          since both are unusually tall) did not fix the
 *                          vertical offset seen on real hardware, and this
 *                          table shows why: 2 was simply wrong.
 *   FBL (Frame Blank Length) = 64 lines (761.33 us) -> frame_blank_lines,
 *                          a new epd_ac_timing_t field (see epd_panel_def.h)
 *                          added specifically because of this panel. Set to
 *                          59, not 64: a separate datasheet note ("first gate
 *                          line on after 5 CKV") accounts for a small gap
 *                          that remained at the true physical top edge with
 *                          64 alone - confirmed on real hardware that the
 *                          correction is 64-5, not 64+5 (69 left the same
 *                          gap in place) - see the field comment inline
 *                          below for the full story.
 *                          frame_gate_start() previously had no way to
 *                          express "N blank lines after sync, before real
 *                          data" - it only ever clocked a fixed 3 positioning
 *                          pulses, enough for every panel validated before
 *                          this one. With 64 real blank lines required and
 *                          only 3 pulses given, the gate driver was nowhere
 *                          near row 0 when real data started - on a 900-row
 *                          panel that is over 7% of the whole frame, which
 *                          is exactly enough to produce "top shifted, bottom
 *                          short of the edge, a blank band in the wrong
 *                          place" rather than a subtle one-line nudge.
 *   FEL (Frame End Length) = 24 lines (285.50 us) -> interframe_us = 286,
 *                          up from the 230 borrowed from ED103TC2. This is
 *                          the blank settle period after the real data,
 *                          before the next frame's sync - what
 *                          interframe_us already models on every panel.
 *   Implied line period ~11.9 us (FSL 1 line = 11.90 us; FBL 64 lines =
 *   761.33 us / 64 = 11.90 us) -> ckv_extra_us = 12, close to that figure and
 *                          used as the spacing for the new frame_blank_lines
 *                          pulses. Previously 18 us, an arbitrary figure
 *                          inherited from ED103TC2 with no connection to this
 *                          panel's own timing.
 *   LSL/LBL (15 + 10 SDCK, leading dummy clocks before each line's real data)
 *                          -> NOT modelled. frame_gate_start()/gate_advance()
 *                          have no concept of leading per-row padding, only
 *                          line_padding_bytes AFTER a row's data. Since the
 *                          reported symptom was purely vertical, this was not
 *                          chased down; if the image is later found shifted
 *                          horizontally by a small constant amount even after
 *                          the vertical fix, this is the first place to look.
 *
 * From the datasheet's AC characteristics table:
 *
 *   fckv    max 200 kHz -> ckv_flush_us = 3 gives a 6 us period (167 kHz).
 *   twL/twH min 0.5 us  -> minimum CKV low/high pulse.  ckv_low_us = 1 for
 *                          2x margin; twH is satisfied by construction (CKV
 *                          held high across each row's DMA and the
 *                          frame-start delays).
 *   tSU/tH  min 100 ns, max twH-100 ns
 *                       -> SPV setup/hold around the CKV edge, cleared by
 *                          the microsecond-scale gate timings.
 *   tcy     min 16.67 ns, typ 50 ns
 *                       -> source clock could reach ~60 MHz; held at 20 MHz
 *                          (the typical figure) to match every other panel
 *                          here for first bring-up.
 *   tstls/tstlh (SPH setup/hold, relative to tcy)
 *                       -> not separately modelled; covered by construction
 *                          the same way tSU/tH are on every other panel.
 *   tLEdly  min 3.5*tcy -> delay before LE rises, 175 ns at the typical tcy.
 *                          Satisfied with room to spare: LE is pulsed only
 *                          after the DMA completion interrupt wakes the
 *                          refresh task, which is microseconds away.
 *   tLEw    300 ns (at VDD 1.7-2.1 V)
 *                       -> THE significant number, identical to ED052TC4's:
 *                          every other panel here asks for 40 ns, satisfied
 *                          by the driver's two back-to-back GPIO writes, but
 *                          300 ns is not, so le_pulse_us is non-zero here
 *                          too - without it the row data is never reliably
 *                          latched onto the source outputs.
 *   tLEoff  min 200 ns  -> covered by the 1 us ckv_low_us gap that follows
 *                          the LE pulse in gate_advance().
 *   tout    max 20 us (200 pF)
 *                       -> source settling, matching ED052TC4's figure and
 *                          comfortably inside the row period.
 *
 * This is, field for field, the same AC profile as ED052TC4 (see that
 * definition's own notes above) - both share ckv_low_us=1, le_pulse_us=1,
 * pclk_hz=20 MHz. Geometry and VCOM are this panel's own.
 *
 * VCOM is read from the panel itself: -2.47V.
 *
 * bus_width=8 per the datasheet's D0..D7. flags defaults to
 * EPD_PANEL_FLAG_NONE, matching every other 8-bit panel on this board (the
 * two bus widths take their first byte from opposite halves of the
 * connector - see ED052TC4's note above) - change to MIRROR_X if the image
 * comes out horizontally flipped.
 *
 * UNVALIDATED, like every panel added since ED103TC2: the waveform block
 * below is copied verbatim from ED103TC2, the longest-validated curve in
 * this file, as the best starting point for a "does it light up, and in the
 * right place" first bring-up - not a claim that it suits this ink. Re-tune
 * against the palette/bars screens once the panel is confirmed working, and
 * only after VCOM and orientation are confirmed - a wrong VCOM looks exactly
 * like a wrong curve.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed067kc1 = {
    .name      = "ED067KC1",

    .width     = 1800,         /* electrical: 1800 px/line - see the note above */
    .height    = 900,          /* electrical: 900 lines/frame - see the note above */
    .bus_width = 8,
    .pclk_hz   = 20000000,     /* datasheet typ tcy 50 ns; min allows ~60 MHz */

    .vcom_mv   = 2470,         /* panel is marked -2.47V */

    /* Matches the other 8-bit panels on this board; check at first bring-up -
     * if text renders mirrored, use EPD_PANEL_FLAG_MIRROR_X instead. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    /* Unconfirmed, as on every panel here without its own bring-up yet - the
     * first thing to change if the image is shifted horizontally by a
     * constant amount. */
    .line_padding_bytes = 16,

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 12,    /* FSL/FBL both imply ~11.9 us/line - see the note above */
        .spv_sync_lines   = 1,     /* FSL = 1 line */
        /*
         * FBL = 64, minus 5 from a separate datasheet note: "first gate line
         * on [is] after 5 CKV". Confirmed on real hardware which direction
         * this goes: 69 (64 + 5) left the same top gap in place, so the
         * note's "5" is measured the other way - gate line 1 is actually
         * reached 5 CKV BEFORE the plain FBL count would suggest, not after.
         * A small gap remained at the true physical top edge with 64 alone
         * too, consistent with a few too many blank pulses still being
         * clocked past row 0's real position before real data starts.
         */
        .frame_blank_lines = 59,
        .ckv_low_us       = 1,     /* twL min 0.5 us */
        .le_pulse_us      = 1,     /* tLEw 300 ns at VDD 1.7-2.1V - required, as on ED052TC4 */

        .interframe_us    = 286,   /* FEL = 24 lines = 285.50 us */
        .row_period_us    = 0,
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under the 200 kHz fckv max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        /*
         * 0, not the 6 every other panel here uses. The neutralising scan
         * (see epd_display_power_off()'s step 2) visibly lightened the image
         * right as the panel powered down - expected per that function's own
         * comments (it drains the residual bias GC16 leaves behind, which is
         * what the lightening actually is), but confirmed on real hardware to
         * be an unwanted trade for this panel: skipping the scan entirely
         * (discharge_frames = 0, per the escape hatch documented at the top
         * of epd_display.c) keeps the image at full contrast through
         * power-down. The cost is losing that scan's protection against
         * image creep/ghosting across many power cycles - worth revisiting
         * if that turns out to matter more than the contrast does.
         */
        .discharge_frames       = 0,
    },

    /* Copied from ED103TC2; re-tune against the bars/palette screens once the
     * panel is confirmed working and VCOM/orientation are settled. */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ES108FC2 — 1920 (H) x 1080 (V), 16-bit source bus, VCOM -1.6V
 *
 * Resolution confirmed two ways: given directly as "1920x1080" in the
 * datasheet's Timing Parameters table, and cross-checked against that same
 * table's own figures - LDL = 240 SDCK x 8 px/SDCK = 1920 px/line,
 * FDL = 1080 lines/frame. Both agree, unlike ED067KC1's mechanical-vs-
 * electrical mismatch - no orientation ambiguity here.
 *
 * pclk_hz is set to this panel's own named operating point, Mode 3 @ 33 MHz,
 * rather than the conservative 20 MHz every earlier panel here defaults to.
 * ED067KC1 cost multiple bring-up rounds partly because its blank-line timing
 * was derived from a 48 MHz table while the panel was actually clocked at
 * 20 MHz, so every "per line" figure was an approximation rather than an
 * exact match. Running the SAME clock the datasheet's Frame/Line Parameters
 * table assumes avoids that reconciliation entirely: at 33 MHz (tcy = 30.3 ns,
 * comfortably above the 16.67 ns floor), 240 SDCK cycles of real row data
 * take exactly 7.27 us - matching the table's own LDL[us] figure precisely.
 *
 * That choice has one real consequence: at 33 MHz this panel's row-data DMA
 * phase alone (~7.3 us) is FASTER than tout's 20 us settling requirement -
 * every slower panel in this file gets tout satisfied for free because its
 * natural row period already exceeds it; this one does not. So unlike every
 * other panel here, row_period_us is set explicitly (22, just over the 20 us
 * max) rather than left at 0 (free-run).
 *
 * From the datasheet's AC characteristics table:
 *
 *   fckv    max 200 kHz -> ckv_flush_us = 3 gives a 6 us period (167 kHz).
 *   twL/twH min 500 ns  -> ckv_low_us = 1 us, 2x margin; twH satisfied by
 *                          construction (CKV held high across the row).
 *   tSU/tH  min 100 ns, max twH-100 ns
 *                       -> cleared by the microsecond-scale gate timings.
 *   tcy     min 16.67 ns -> pclk_hz = 33 MHz (tcy 30.3 ns) - see above.
 *   tsu/th on D0..D7, tstls/tstlh (XSTL setup/hold - this datasheet's own
 *   name for what other panels' tables call SPH, matching this board's own
 *   XSTL pin name)
 *                       -> not separately modelled; peripheral-timed, same
 *                          as every other panel here.
 *   tLEdly  min 10.5*tcy -> 318 ns at 33 MHz. Satisfied with room to spare:
 *                          LE only fires after the DMA-completion interrupt
 *                          wakes the refresh task, microseconds away.
 *   tLEw    300 ns (at VDD 1.7-2.1 V)
 *                       -> le_pulse_us = 1, same as ED052TC4/ED067KC1. First
 *                          time this requirement has shown up on a 16-bit
 *                          panel rather than an 8-bit one - the two
 *                          properties are apparently unrelated.
 *   tLEoff  min 200 ns  -> covered by the 1 us ckv_low_us gap after LE.
 *   tout    max 20 us   -> NOT satisfied by construction here - see above.
 *                          row_period_us = 22 makes it explicit instead.
 *
 * From the datasheet's Timing Parameters table (Mode 3, SDCK = 33 MHz):
 *
 *   FSL (Frame Start Length) = 1 line (10.70 us) -> spv_sync_lines = 1.
 *   FBL (Frame Blank Length) = 4 lines (42.79 us) -> frame_blank_lines = 4.
 *                          Per-line figure (42.79/4 = 10.70 us) matches FSL's
 *                          own 1-line figure exactly, giving ckv_extra_us
 *                          below. Much smaller than ED067KC1's 59-64: take
 *                          this as the datasheet's number, not yet confirmed
 *                          on hardware - ED067KC1 also needed an undocumented
 *                          +/-5 correction on top of its own FBL, so treat 4
 *                          as a starting point for first bring-up, the same
 *                          way every panel's spv_sync_lines/frame_blank_lines
 *                          has been until actually driven on real glass.
 *   FEL (Frame End Length) = 15 lines (160.45 us) -> interframe_us = 161.
 *   Implied line period ~10.70 us (FSL and FBL/4 agree) -> ckv_extra_us = 11.
 *   LSL/LBL (10 + 7 SDCK, leading dummy clocks before each line's real data)
 *                       -> NOT modelled, same known gap as ED067KC1: this
 *                          driver has no concept of per-row leading padding,
 *                          only line_padding_bytes AFTER a row. Worth
 *                          checking first if the image is shifted
 *                          horizontally by a small constant amount.
 *
 * Everything else - power sequencing, drive codes, line_padding_bytes,
 * orientation flags, the waveform/tone curve, discharge_frames - is copied
 * from ED103TC2 as instructed, since none of it is in either table above and
 * ED103TC2 is the longest-validated definition in this file. All of it is
 * therefore UNVALIDATED for this panel's own ink/wiring and should be
 * checked at first bring-up, same as every panel added after ED103TC2:
 * flags may need to become EPD_PANEL_FLAG_NONE if the image comes out
 * mirrored, and the waveform will need re-tuning against the bars/palette
 * screens once VCOM and orientation are confirmed correct - a wrong VCOM
 * looks exactly like a wrong curve.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_es108fc2 = {
    .name      = "ES108FC2",

    .width     = 1920,
    .height    = 1080,
    .bus_width = 16,
    .pclk_hz   = 33000000,    /* this panel's own Mode 3 - see the note above */

    .vcom_mv   = 1600,         /* panel is marked -1.6V */

    /* Copied from ED103TC2 per instruction - check at first bring-up, same as
     * every panel here that hasn't been driven on real glass yet. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    .line_padding_bytes = 16,   /* copied from ED103TC2; unconfirmed for this panel */

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 11,    /* FSL/FBL both imply ~10.70 us/line - see the note above */
        .spv_sync_lines   = 1,     /* FSL = 1 line */
        .frame_blank_lines = 4,    /* FBL = 4 lines - see the note above */
        .ckv_low_us       = 1,     /* twL min 500 ns */
        .le_pulse_us      = 1,     /* tLEw 300 ns at VDD 1.7-2.1V - required, as on ED052TC4/ED067KC1 */

        .interframe_us    = 161,   /* FEL = 15 lines = 160.45 us */
        .row_period_us    = 22,    /* tout max 20 us is NOT free - see the note above */
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under the 200 kHz fckv max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 0,
    },

    /* Copied from ED103TC2; re-tune against the bars/palette screens once the
     * panel is confirmed working and VCOM/orientation are settled. */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ES120MC1 (VD1400-MOA) — 2560 (H) x 1600 (V), 16-bit source bus, VCOM -1.6V
 *
 * Same reference design family as ES108FC2 - identical tLEdly (10.5*tcy),
 * same document series ("VD1400-MOA"), same FSL=1/FBL=4 - just a different
 * resolution/clock variant. Resolution confirmed both ways again: stated
 * directly as "2560x1600" in the Timing Parameters table, and cross-checked
 * against that table's own figures - LDL = 320 SDCK x 8 px/SDCK = 2560
 * px/line, FDL = 1600 lines/frame.
 *
 * pclk_hz = 44 MHz, this panel's own Mode 3, for the same reason as
 * ES108FC2: keeping every derived timing figure consistent with the clock
 * the datasheet's own Line/Frame Parameters table is built around, rather
 * than reconciling against an unrelated conservative default. 320 SDCK
 * cycles at 44 MHz (tcy = 22.7 ns) is 7.27 us, matching the table's own
 * LDL[us] figure exactly.
 *
 * Same tout consequence as ES108FC2: the row-data DMA phase alone (~7.3 us)
 * is faster than tout's 20 us settling max, so row_period_us is set
 * explicitly (22) rather than left at 0.
 *
 * From the datasheet's AC characteristics table (VDD 3.0-3.6V unless noted):
 *
 *   fckv    max 200 kHz -> ckv_flush_us = 3 gives a 6 us period (167 kHz).
 *   twL/twH min 500 ns  -> ckv_low_us = 1 us, 2x margin; twH satisfied by
 *                          construction (CKV held high across the row).
 *   tSU/tH  min 100 ns, max twH-100 ns
 *                       -> cleared by the microsecond-scale gate timings.
 *   tcy     min 16.67 ns -> pclk_hz = 44 MHz (tcy 22.7 ns) - see above.
 *   tsu/th on D0..D15 (confirms the 16-bit bus), tstls/tstlh (SPH setup/hold)
 *                       -> not separately modelled; peripheral-timed, same
 *                          as every other panel here.
 *   tLEdly  min 10.5*tcy -> 238 ns at 44 MHz. Satisfied with room to spare,
 *                          same reasoning as every other panel: LE only
 *                          fires after the DMA-completion interrupt wakes
 *                          the refresh task, microseconds away.
 *   tLEw    300 ns (at VDD 2.5-3.6 V here, vs ES108FC2's 1.7-2.1V - same
 *                    300 ns figure either way)
 *                       -> le_pulse_us = 1, same as ED052TC4/ED067KC1/ES108FC2.
 *   tLEoff  min 200 ns  -> covered by the 1 us ckv_low_us gap after LE.
 *   tout    max 20 us   -> NOT satisfied by construction here - see above.
 *                          row_period_us = 22 makes it explicit instead.
 *
 * From the datasheet's Timing Parameters table (Mode 3, SDCK = 44 MHz):
 *
 *   FSL (Frame Start Length) = 1 line (8.22 us) -> spv_sync_lines = 1.
 *   FBL (Frame Blank Length) = 4 lines (32.86 us) -> frame_blank_lines = 4.
 *                          Same FBL line count as ES108FC2, consistent with
 *                          this being the same reference TCON design. Per-
 *                          line figure (32.86/4 = 8.22 us) matches FSL's own
 *                          1-line figure exactly, giving ckv_extra_us below.
 *                          Still a datasheet number, not yet confirmed on
 *                          hardware - treat as a first-bring-up starting
 *                          point the same way ES108FC2's is, given
 *                          ED067KC1 needed an undocumented +/-5 correction
 *                          on top of its own FBL.
 *   FEL (Frame End Length) = 18 lines (147.89 us) -> interframe_us = 148.
 *   Implied line period ~8.22 us (FSL, FBL/4, and FEL/18 all agree)
 *                       -> ckv_extra_us = 9.
 *   LSL/LBL (14 + 10 SDCK, leading dummy clocks before each line's real
 *   data) -> NOT modelled, same known gap as ED067KC1/ES108FC2: this driver
 *                          has no concept of per-row leading padding, only
 *                          line_padding_bytes AFTER a row.
 *
 * Everything else - power sequencing, drive codes, line_padding_bytes,
 * orientation flags, the waveform/tone curve, discharge_frames - is copied
 * from ED103TC2, the longest-validated definition in this file, following
 * the same convention as ES108FC2 (not explicitly requested this time, but
 * nothing else here supplies these and this panel's AC profile is close
 * enough to ES108FC2's to treat the same way). All of it is UNVALIDATED for
 * this panel's own ink/wiring and should be checked at first bring-up: flags
 * may need EPD_PANEL_FLAG_NONE if the image comes out mirrored, and the
 * waveform will need re-tuning once VCOM and orientation are confirmed -
 * a wrong VCOM looks exactly like a wrong curve.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_es120mc1 = {
    .name      = "ES120MC1",

    .width     = 2560,
    .height    = 1600,
    .bus_width = 16,
    .pclk_hz   = 44000000,    /* this panel's own Mode 3 - see the note above */

    .vcom_mv   = 500,         /* panel is marked -1.6V */

    /* Copied from ED103TC2 - check at first bring-up, same as every panel
     * here that hasn't been driven on real glass yet. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    .line_padding_bytes = 16,   /* copied from ED103TC2; unconfirmed for this panel */

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,
        .spv_low_us       = 10,
        .ckv_post_spv_us  = 8,
        .spv_high_us      = 10,
        .ckv_extra_us     = 9,     /* FSL/FBL/FEL all imply ~8.22 us/line - see the note above */
        .spv_sync_lines   = 1,     /* FSL = 1 line */
        .frame_blank_lines = 4,    /* FBL = 4 lines - see the note above */
        .ckv_low_us       = 1,     /* twL min 500 ns */
        .le_pulse_us      = 1,     /* tLEw 300 ns - required, as on ED052TC4/ED067KC1/ES108FC2 */

        .interframe_us    = 148,   /* FEL = 18 lines = 147.89 us */
        .row_period_us    = 22,    /* tout max 20 us is NOT free - see the note above */
        .ckv_flush_us     = 3,     /* 6 us period = 167 kHz, under the 200 kHz fckv max */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 0,
    },

    /* Copied from ED103TC2; re-tune against the bars/palette screens once the
     * panel is confirmed working and VCOM/orientation are settled. */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED078KC1 - 7", 1872x1404, 16-bit source bus
 *
 * Same resolution/bus width as ED103TC2 (per the person bringing this panel
 * up, who has one physically in hand) - added specifically to test whether
 * the black-to-white driving problems found on this board's ED103TC2 unit
 * (weak/broken lightening across differential DU, differential GC16, and
 * windowed INIT - see the comments on waveform_du()/waveform_gc16()/
 * waveform_init() in epd_display.c) are a property of that specific piece of
 * glass or of the driving scheme in general.
 *
 * STILL PENDING, not yet provided:
 *   - VCOM. No value has been read from this panel's FPC ribbon yet - vcom_mv
 *     below is copied from ED103TC2 purely as a non-zero placeholder and MUST
 *     be checked before this definition is trusted for anything, the same as
 *     every "copied" field below.
 *   - This panel's own Timing Parameters table (FSL/FBL/FEL frame-level
 *     timing) - a second datasheet page still to come. spv_sync_lines,
 *     ckv_extra_us, interframe_us and discharge_frames are ED103TC2's own
 *     empirically-validated figures, not yet derived from this panel's own
 *     numbers - update them once that page arrives, the same way ES108FC2/
 *     ES120MC1's FSL/FBL/FEL came from their own second table rather than
 *     being left at ED103TC2's.
 *
 * From the datasheet's AC characteristics table (the one page provided so far):
 *
 *   fckv    max 200 kHz -> ckv_flush_us = 3 gives a 6 us period (167 kHz),
 *                          same figure as ED103TC2.
 *   twL/twH min 500 ns  -> ckv_low_us = 1 us, 2x margin (ED103TC2 itself uses
 *                          0, "validated with back-to-back CKV edges", but
 *                          that is a claim about ED103TC2 specifically - this
 *                          panel keeps the safer margin until it has its own
 *                          bring-up history).
 *   tSU/tH  min 100 ns  -> cleared by the microsecond-scale gate timings.
 *   tcy     min 16.67 ns, typ 50 ns -> pclk_hz = 20 MHz (tcy = 50 ns exactly)
 *                          - the same clock ED103TC2 already uses, so this is
 *                          not a new assumption.
 *   tsu/th on D0..D7, tstls/tstlh (SPH setup/hold)
 *                       -> not separately modelled; peripheral-timed, same as
 *                          every other panel here.
 *   tLEdly  min 10.5*tcy -> 525 ns at 20 MHz. Satisfied with room to spare -
 *                          LE only fires after the DMA-completion interrupt
 *                          wakes the refresh task, microseconds away.
 *   tLEw    300 ns (at VDD 1.7-2.1V) -> le_pulse_us = 1, same as
 *                          ED052TC4/ED067KC1/ES108FC2/ES120MC1.
 *   tLEoff  min 200 ns  -> covered by the 1 us ckv_low_us gap after LE.
 *   tout    max 20 us   -> row_period_us left at 0 (free-run), same as
 *                          ED103TC2: same 20 MHz clock, so the same "proved
 *                          insensitive to it" reasoning applies until bring-up
 *                          says otherwise, unlike ES108FC2/ES120MC1's faster
 *                          clocks which needed row_period_us set explicitly.
 *
 * Everything else - power sequencing, drive codes, orientation flags, the
 * waveform/tone curve - is copied from ED103TC2 verbatim, per the same
 * convention as every panel added since ED103TC2. All of it is UNVALIDATED
 * for this panel's own ink/wiring: flags may need to change if the image
 * comes out mirrored, and the waveform will need re-tuning once VCOM and
 * orientation are confirmed - a wrong VCOM looks exactly like a wrong curve.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed078kc1 = {
    .name      = "ED078KC1",

    .width     = 1872,
    .height    = 1404,
    .bus_width = 16,
    .pclk_hz   = 20000000,

    .vcom_mv   = 1870,

    /* Copied from ED103TC2 - check at first bring-up, same as every panel
     * here that hasn't been driven on real glass yet. */
    .flags     = EPD_PANEL_FLAG_NONE,

    .temp_compensation = true,

    .line_padding_bytes = 16,   /* copied from ED103TC2; unconfirmed for this panel */

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,     /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .spv_low_us       = 10,    /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .ckv_post_spv_us  = 8,     /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .spv_high_us      = 10,    /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .ckv_extra_us     = 18,    /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .spv_sync_lines   = 1,     /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .ckv_low_us       = 1,     /* twL/twH min 500 ns, 2x margin - see note above */
        .le_pulse_us      = 1,     /* tLEw 300 ns - see note above */

        .interframe_us    = 230,   /* copied from ED103TC2 - pending this panel's own Timing Parameters table */
        .row_period_us    = 0,     /* free-run; tout satisfied by construction at this clock - see note above */
        .ckv_flush_us     = 3,     /* keeps CKV inside its 200 kHz maximum */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 6,
    },

    /* Copied from ED103TC2; re-tune against the bars/palette screens once the
     * panel is confirmed working and VCOM/orientation are settled. */
    .wf = {
        .gc16_phases     = 4,
        .gc16_phase_cut  = { 16, 32, 48, 64 },
        .gc16_phase_order = { 0, 1, 2, 3 },

        .gc16_level_energy = {
            64, 50, 38, 34, 30, 26, 22, 19,
            16, 13, 10,  8,  6,  4,  2,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        .init_group_frames = 4,

        .du_frames         = 8,
        .du_dark_threshold = 8,
    },
};

/*******************************************************************************
 * E Ink ED140TT1 - 1440x300, 8-bit source bus, VCOM -1.80V (read from the
 * panel's own FPC ribbon)
 *
 * A "bar" aspect panel like ED067KC1 - very short/wide (1440x300) rather
 * than the roughly-square panels most of this file targets.
 *
 * width=1440/height=300 here are the ELECTRICAL scan direction, confirmed
 * against the full datasheet's "CLOCK & DATA TIMING" diagram, which labels
 * the source driver outputs explicitly as "OUT1~OUT1440" - 1440 source
 * (data) columns, 300 gate (CKV-scanned) rows. This is NOT what the
 * datasheet's own "Display Resolution: 300(H)x1440(V)" table entry
 * suggests - that describes the panel's mechanical/mounting orientation,
 * not scan direction, the exact same trap ED067KC1's own comment in this
 * file already warns about ("width=1800/height=900 here are the ELECTRICAL
 * scan direction... not the 900x1800 the mechanical spec's H/V labelling
 * suggests").
 *
 * Pin 33 on the 40-pin FPC is a separate "Border" signal, not modelled in
 * epd_board_config_t (no field for it) - typically tied to VCOM externally
 * on the board. Worth checking that wiring; a floating border ring is a
 * plausible source of edge-only artifacts.
 *
 * From the datasheet's AC characteristics table:
 *
 *   fckv    max 200 kHz -> ckv_flush_us = 3 gives a 6 us period (167 kHz),
 *                          same figure as every other panel here.
 *   twL/twH min 500 ns  -> ckv_low_us = 1 us, 2x margin - same reasoning as
 *                          ED078KC1: no bring-up history yet to justify
 *                          ED103TC2's own back-to-back-edges 0.
 *   tSU/tH  min 100 ns, max twH-100 ns -> cleared by the microsecond-scale
 *                          gate timings.
 *   tcy     min 16.67 ns, no typical given -> pclk_hz = 20 MHz (tcy = 50 ns),
 *                          the same conservative default ED103TC2/ED078KC1
 *                          use absent a documented faster Mode for this
 *                          panel.
 *   tsu/th on D0..D7 (confirms the 8-bit bus), tstls/tstlh (SPH setup/hold)
 *                       -> not separately modelled; peripheral-timed, same
 *                          as every other panel here.
 *   tLEdly  min 3.5*tcy -> 175 ns at 20 MHz. Satisfied with room to spare -
 *                          LE only fires after the DMA-completion interrupt
 *                          wakes the refresh task, microseconds away.
 *   tLEw    40 ns (at VDD 2.73-3.6V) -> le_pulse_us = 0. An order of
 *                          magnitude under the ~300 ns figure that forced
 *                          ED052TC4/ED067KC1/ED078KC1/ES108FC2/ES120MC1 to
 *                          widen this explicitly - 40 ns is already covered
 *                          by construction, the same reasoning ED103TC2
 *                          itself uses for le_pulse_us = 0.
 *   tLEoff  min 200 ns  -> covered by the 1 us ckv_low_us gap after LE.
 *   tout    max 12 us   -> row_period_us left at 0 (free-run). At 20 MHz an
 *                          8-bit bus shifts this panel's 720 output bytes/row
 *                          (1440 px, 2 px/byte) in ~36 us - already well
 *                          past tout before the row period logic even gets
 *                          involved, so there is nothing to pad. Contrast
 *                          ES108FC2/ES120MC1, whose much faster clocks made
 *                          the DMA phase alone faster than tout and forced
 *                          row_period_us to be set explicitly.
 *
 * No full Timing Parameters table was supplied, but a "Frame Sync Length"
 * diagram was given separately, explicitly labelling the SPV-low duration
 * "t1" - the exact datasheet symbol spv_sync_lines is documented against
 * (see epd_panel_def.h). That diagram draws t1 spanning two CKV pulses
 * before the regular pulse train continues, so spv_sync_lines = 2 here (the
 * same non-default value only ED115OC1 has otherwise needed).
 *
 * The diagram's own accompanying note - "after 5CKV, gate line is on" -
 * checks out against that: frame_gate_start() in epd_display.c always walks
 * a further fixed 3 pulses after the sync window before the gate reaches
 * row 0, so 2 (spv_sync_lines) + 3 (fixed walk) = 5, matching the note
 * exactly with frame_blank_lines left at its default 0 - no extra fudge
 * needed. (An earlier version of this comment read the note alone, without
 * yet having this diagram, and set spv_sync_lines = 5 outright - wrong, kept
 * here as a record of the correction.)
 *
 * ckv_extra_us/interframe_us/discharge_frames still have nothing to derive
 * them from beyond ED103TC2's own empirically-validated figures - copied as
 * placeholders, same convention as ED078KC1's pending frame-level timing.
 *
 * Everything else - power sequencing, drive codes, orientation flags, the
 * waveform/tone curve - is copied from ED103TC2 verbatim, per the same
 * convention as every panel added since ED103TC2. All of it is UNVALIDATED
 * for this panel's own ink/wiring: flags may need to change if the image
 * comes out mirrored, and the waveform will need re-tuning once orientation
 * is confirmed on real hardware.
 ******************************************************************************/
const epd_panel_def_t epd_panel_eink_ed140tt1 = {
    .name      = "ED140TT1",

    .width     = 1440,
    .height    = 300,
    .bus_width = 8,
    .pclk_hz   = 20000000,
    .vcom_mv   = 1860,          /* panel is marked -1.80V */
    .flags     = EPD_PANEL_FLAG_MIRROR_Y,
    .temp_compensation = true,
    .line_padding_bytes = 16,   /* copied from ED103TC2; unconfirmed for this panel */

    .power = {
        .vee_strobe  = 0,
        .vneg_strobe = 1,
        .vpos_strobe = 2,
        .vddh_strobe = 3,
        .delay_per_strobe_ms = 9,
    },

    .codes = {
        .no_drive = 0x00u,   /* grounded: settling scans only        */
        .darken   = 0x01u,   /* VNEG                                 */
        .lighten  = 0x02u,   /* VPOS                                 */
        .hold     = 0x03u,   /* leave the pixel alone                */
    },

    .timing = {
        .ckv_pre_spv_us   = 7,     /* copied from ED103TC2 - no Timing Parameters table supplied */
        .spv_low_us       = 10,    /* copied from ED103TC2 - no Timing Parameters table supplied */
        .ckv_post_spv_us  = 8,     /* copied from ED103TC2 - no Timing Parameters table supplied */
        .spv_high_us      = 10,    /* copied from ED103TC2 - no Timing Parameters table supplied */
        .ckv_extra_us     = 18,    /* copied from ED103TC2 - no Timing Parameters table supplied */
        .spv_sync_lines   = 2,     /* datasheet "Frame Sync Length" diagram: t1 = 2 CKV lines - see note above */
        .ckv_low_us       = 1,     /* twL/twH min 500 ns, 2x margin - see note above */
        .le_pulse_us      = 0,     /* tLEw 40 ns - satisfied by construction, see note above */

        .interframe_us    = 230,   /* copied from ED103TC2 - no Timing Parameters table supplied */
        .row_period_us    = 0,     /* free-run; tout satisfied by construction at this clock/bus - see note above */
        .ckv_flush_us     = 3,     /* keeps CKV inside its 200 kHz maximum */

        .vcom_off_settle_ms     = 60,
        .post_discharge_wait_ms = 500,
        .global_discharge_ms    = 250,
        .discharge_frames       = 0,
    },

    /* Copied from ED103TC2; re-tune against the bars/palette screens once the
     * panel is confirmed working and orientation is settled. */
    .wf = {
        /* Raised 4 -> 8 phases (EPD_GC16_MAX_PHASES, the driver's ceiling):
         * user-observed black still not dark enough under GC16 even after
         * rebalancing gc16_level_energy - at 4 phases the blackest level
         * already saturates all of them (energy 64 == the last cut), so
         * there was no more darkening left to give it within that phase
         * count. Doubling phases keeps the same 4x4-tiled-2x2 Bayer matrix
         * (still spaced by max_bayer+1 = 16 per phase, per the cut-spacing
         * rule in epd_panel_def.h) but doubles the total number of darken
         * passes the blackest level can receive - the same "more total
         * drive dose" fix as du_frames/init_group_frames above, this time
         * for GC16. Doubles GC16 refresh time in exchange. */
        .gc16_phases     = 8,
        .gc16_phase_cut  = { 16, 32, 48, 64, 80, 96, 112, 128 },
        .gc16_phase_order = { 0, 1, 2, 3, 4, 5, 6, 7 },

        /* Same linear-ramp shape as before, doubled to span the new 0-128
         * range instead of 0-64, so every level's phase count doubles along
         * with the ceiling rather than only the blackest one. */
        .gc16_level_energy = {
            128, 120, 110, 102, 94, 86, 76, 68,
             60,  52,  42,  34, 26, 18,  8,  0,
        },

        .bayer = {
            /* classic 4x4 dither tiled 2x2: with cuts 16/32/48/64 each
             * value appears four times in the 64 cells, so drive works
             * out to energy/16 exactly as with a true 4x4 matrix */
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
            {  0,  8,  2, 10,  0,  8,  2, 10 },
            { 12,  4, 14,  6, 12,  4, 14,  6 },
            {  3, 11,  1,  9,  3, 11,  1,  9 },
            { 15,  7, 13,  5, 15,  7, 13,  5 },
        },

        /* init_group_frames raised 4 -> 8 (ED103TC2's own untuned value):
         * user-observed ghosting on real hardware when switching between
         * full-screen pages (old page's text ghosts through the new one) -
         * i.e. EPD_WAVEFORM_INIT itself isn't fully clearing before the next
         * page is drawn on top, the same "not enough drive" failure the
         * comment above waveform_init() in epd_display.c documents measuring
         * on ED103TC2 (2 groups/8 frames left ghosting, 4 groups/16 frames was
         * clean). Doubling it here is the same experiment for this panel's
         * own, still-uncharacterised ink. If ghosting persists, try 12 next;
         * each step doubles INIT's clear time (~1.2 s at 4, so ~2.4 s at 8). */
        .init_group_frames = 8,

        /* du_frames raised 8 -> 16 (every other panel here still has
         * ED103TC2's own untuned 8): user-observed ghosting on real hardware
         * from DU transitions happening too fast for this panel's ink to
         * fully switch before the driver moves on. du_frames is exactly the
         * total-dwell knob for DU - the driver holds darken/lighten on a
         * pixel for this many complete frame passes, so doubling it doubles
         * how long each pixel is actually driven, at the cost of a slower
         * update. If ghosting persists, try 24-32 next; if it disappears well
         * before 16, it can come back down. */
        .du_frames         = 16,
        .du_dark_threshold = 8,
    },
};
