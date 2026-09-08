#ifndef EPD_PANELS_H
#define EPD_PANELS_H

#include "epd_panel_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Panel catalogue
 *
 * Add a new panel by adding a definition in epd_panels.c and declaring it here,
 * then pass it to epd_display_panel_create().  Nothing in the driver needs to
 * change.
 *
 * The waveform block of each definition is empirically tuned for that specific
 * panel's ink and driver ICs and does not carry over to another one.  Start
 * from the closest existing definition and re-tune against the calibration
 * screens in main.c.
 ******************************************************************************/

/**
 * ED103TC2 10.3", 1872x1404 16-bit parallel E Ink panel (the EPDInky P4 reference panel).
 *
 * This is the definition the driver was developed and tuned against; every
 * measurement quoted in epd_display.c refers to it.
 */
extern const epd_panel_def_t epd_panel_eink_ed103tc2;

/**
 * E Ink ED097TC2 - 9.7", 1200x825, 8-bit source bus.
 *
 * UNVALIDATED: geometry and AC timing are from the datasheet, but the waveform
 * block is inherited from the 1872x1404 panel and VCOM is a placeholder.  See
 * the notes on the definition in epd_panels.c before using it.
 */
extern const epd_panel_def_t epd_panel_eink_ed097tc2;

/**
 * E Ink ED113TC1 - 2400x1034, 16-bit source bus, VCOM -1.2 V.
 *
 * A near-copy of the ED103TC2 definition differing only in geometry and two
 * datasheet timings, so that driving it isolates whether a blank ED097TC2 is
 * an 8-bit problem or a driver-wide one.
 */
extern const epd_panel_def_t epd_panel_eink_ed113tc1;

/**
 * E Ink ED060SCP - 6", 600x800, 8-bit source bus, VCOM -1.6 V.
 *
 * The second 8-bit panel, kept close to ED103TC2 so that a blank result here
 * distinguishes an 8-bit path problem from an ED097TC2-specific one.
 */
extern const epd_panel_def_t epd_panel_eink_ed060scp;

/**
 * E Ink ED052TC4 - 720x1280, 8-bit source bus, VCOM -2.57 V.
 *
 * The first panel here to require a widened LE pulse (tLEw 300 ns against the
 * 40 ns of the others) - see timing.le_pulse_us.
 */
extern const epd_panel_def_t epd_panel_eink_ed052tc4;

/**
 * E Ink ED115OC1 - 2760x2070, 16-bit source bus, VCOM -1.34 V.
 *
 * The largest panel here (2.86 MB framebuffer) and the first to require a
 * two-line frame sync - see timing.spv_sync_lines.
 */
extern const epd_panel_def_t epd_panel_eink_ed115oc1;

/**
 * E Ink ED133UT2 - 2200x1650, 16-bit bus, VCOM -1.89V.
 * 13.3" A4-format panel (Sony DPT-RP1).  Untested on this board.
 */
extern const epd_panel_def_t epd_panel_eink_ed133ut2;

/**
 * E Ink ED070KH1 - 1680x1264, 7".
 * No datasheet: AC timing copied from ED103TC2, and bus_width and vcom_mv are
 * assumptions.  See the note above the definition before bring-up.
 */
extern const epd_panel_def_t epd_panel_eink_ed070kh1;

/**
 * E Ink ED067KC1 - 6.7" bar panel, VCOM -2.47V (read from the panel itself).
 *
 * width=1800/height=900 here are the ELECTRICAL scan direction (from the
 * datasheet's Timing Parameters table), not the 900x1800 the mechanical
 * spec's H/V labelling suggests - that describes mounting orientation, not
 * scan direction. Also needed frame_blank_lines=59 (a new epd_ac_timing_t
 * field this panel introduced) to fix a real vertical offset seen on
 * hardware - see the note above the definition in epd_panels.c for the full
 * story, it took several rounds of bring-up to get all of this right,
 * including which direction the datasheet's own correction note went.
 *
 * AC timing profile otherwise matches ED052TC4 field-for-field (tLEw 300 ns
 * at VDD 1.7-2.1V requires le_pulse_us) - both are apparently the same
 * generic 8-bit TCON interface.  UNVALIDATED: the waveform block is
 * inherited from ED103TC2.  See the note above the definition in
 * epd_panels.c before bring-up.
 */
extern const epd_panel_def_t epd_panel_eink_ed067kc1;

/**
 * E Ink ES108FC2 - 1920x1080, 16-bit source bus, VCOM -1.6V (read from the
 * panel itself).
 *
 * pclk_hz is this panel's own Mode 3 (33 MHz), not the conservative 20 MHz
 * every earlier panel here defaults to - see the note above the definition
 * in epd_panels.c for why, and for the row_period_us=22 that choice makes
 * necessary (this is the first panel in this file whose row period does not
 * satisfy tout for free).
 *
 * spv_sync_lines/frame_blank_lines/ckv_extra_us/interframe_us come from this
 * panel's own Timing Parameters table (FSL/FBL/FEL); everything else -
 * power sequencing, drive codes, orientation, the waveform - is copied from
 * ED103TC2 and UNVALIDATED, per the same convention as every panel added
 * since ED103TC2. See the note above the definition in epd_panels.c before
 * bring-up.
 */
extern const epd_panel_def_t epd_panel_eink_es108fc2;

/**
 * E Ink ES120MC1 (VD1400-MOA) - 2560x1600, 16-bit source bus, VCOM -1.6V
 * (read from the panel itself).
 *
 * Same reference design family as ES108FC2 (same document series, same
 * tLEdly multiplier, same FSL/FBL line counts) - just a different
 * resolution/clock variant. pclk_hz is this panel's own Mode 3 (44 MHz),
 * for the same reason and with the same row_period_us=22 consequence as
 * ES108FC2 - see the note above the definition in epd_panels.c.
 *
 * spv_sync_lines/frame_blank_lines/ckv_extra_us/interframe_us come from this
 * panel's own Timing Parameters table (FSL/FBL/FEL); everything else -
 * power sequencing, drive codes, orientation, the waveform - is copied from
 * ED103TC2 and UNVALIDATED, per the same convention as every panel added
 * since ED103TC2. See the note above the definition in epd_panels.c before
 * bring-up.
 */
extern const epd_panel_def_t epd_panel_eink_es120mc1;

/**
 * E Ink ED078KC1 - 7", 1872x1404, 16-bit source bus.
 *
 * Same resolution/bus width as ED103TC2 - added to test whether the
 * black-to-white driving problems found on this board's ED103TC2 unit
 * (differential DU/GC16 and windowed INIT all showed anomalies pushing a
 * pixel toward white) are specific to that piece of glass or general to the
 * driving scheme. UNVALIDATED: VCOM is a placeholder and this panel's own
 * frame-level (FSL/FBL/FEL) timing hasn't been supplied yet - everything
 * beyond the AC characteristics table is copied from ED103TC2. See the note
 * above the definition in epd_panels.c before bring-up.
 */
extern const epd_panel_def_t epd_panel_eink_ed078kc1;

#ifdef __cplusplus
}
#endif

#endif /* EPD_PANELS_H */
