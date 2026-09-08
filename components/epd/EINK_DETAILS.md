# How the `epd` component drives a raw E Ink panel

This document explains, in detail, how `components/epd` drives a **raw** E Ink
panel — bare glass plus source/gate driver ICs, no timing controller — from an
ESP32 chip's LCD i80 peripheral. It covers the electrical model, the frame/row
driving sequence, how a 4-bit greyscale image is turned into a waveform of
2-bit drive pulses (the GC16 "tone model"), how that turns into runtime lookup
tables (LUTs), and the DU and INIT waveforms. It is a companion to the source,
not a replacement for it — file:line references are given throughout so you
can jump straight to the implementation.

Primary files:

| File | Role |
|---|---|
| [epd_panel_def.h](include/epd_panel_def.h) | The panel *contract*: geometry, AC timing, drive codes, waveform/tone model |
| [epd_display.c](src/epd_display.c) | The driver: turns a panel definition into GPIO/DMA activity |
| [epd_i80_bus.c](src/epd_i80_bus.c) | Bottom layer: LCD i80 peripheral (DMA) + bit-banged control GPIOs |
| [epd_panel.c](src/epd_panel.c) | Generic lifecycle/dispatch shared by any panel driver |
| [epd_panels.c](src/epd_panels.c) | The catalogue: concrete, empirically-tuned panel definitions |
| [epd_fb.c](src/epd_fb.c) | 4bpp application framebuffer (drawing, text, images) |

---

## 1. Why a raw EPD needs all of this

A "raw" E Ink panel (as opposed to one with an onboard TCON such as the ones
found in ordinary consumer e-readers with an SPI interface) exposes its gate
driver and source driver shift registers directly. There is no chip on the
glass that turns a command byte into a waveform — the host has to:

1. Generate every gate-clock edge that walks the "select this row" token down
   the panel (the **gate driver**, controlled by `CKV`/`SPV`).
2. Shift 2-bit-per-pixel drive codes into the **source driver** shift register
   for the currently selected row, and latch them onto the row's pixels with
   `LE` (`XLE`) while `SPH` (`XSTL`) frames the transfer.
3. Do this **once per row, every frame, for every frame of the waveform** —
   E Ink is inherently a multi-frame (temporal) technology: a single "drive
   pulse" only nudges the charged pigment particles a little, so a full
   black/white transition takes several frames of driving, and a grey level
   is built up from *how many* of those frames drive a given pixel.

So "driving an EPD" is really two nested loops: an outer loop over **frames**
of a waveform (e.g. 4 GC16 phases, or 8 DU frames), and an inner loop over
**rows** within each frame. The `epd` component's whole structure follows
from that.

The chip's LCD i80 peripheral supplies the *source* side: it DMAs pixel bytes
out over the parallel data bus at a fixed pixel clock (`CL`/`WR`), with `SPH`
as its hardware chip-select. Everything else — `CKV`, `SPV`, `LE`, `OE`,
`GMOD` — is bit-banged GPIO, sequenced by the driver in software around each
DMA row transfer.

---

## 2. Signal map

```
Source bus (DMA, hardware-timed):
  D0..D15  -> data_pins[]     2 bits per pixel, 4 (8-bit bus) or 8 (16-bit bus)
                               pixels per transferred word
  CL       -> pin_cl          source shift clock == LCD peripheral's WR strobe
  SPH      -> pin_sph         source start pulse == LCD peripheral's hardware CS
                               (goes low at transfer start, high at transfer end —
                               never bit-banged, see note in epd_i80_bus.c)

Gate side (bit-banged GPIO, software-timed):
  CKV      -> pin_ckv         gate shift clock (max ~200 kHz)
  SPV/STV  -> pin_spv         gate start pulse — active LOW; injects a "select
                               row 0" token into the gate shift register on
                               the next CKV edge while held low
  LE/XLE   -> pin_le          source latch enable — commits the shift
                               register's contents onto the row's pixel lines
  OE       -> pin_oe          source output enable (polarity: oe_active_high)
  GMOD     -> pin_gmod        gate driver mode/reset

PMIC (owned by the tps65185 driver, never touched by epd_i80_bus):
  WAKEUP, PWR_GOOD, VCOM_CTRL, and the four power rails VDDH/VPOS/VEE/VNEG
```

`dc_dummy` deserves a special mention: an EPD has no command/data
distinction, but the ESP32 LCD i80 peripheral (IDF ≥ 6.0) refuses to be
configured without a DC GPIO. The driver points it at a spare, genuinely
unconnected pin; `lcd_cmd_bits = 0` means the peripheral never actually
toggles it, but it *is* driven continuously, so wiring it to something else
on the board (e.g. an interrupt line) causes silent contention. See
[epd_board.h:54-67](include/epd_board.h#L54-L67).

Board wiring (`epd_board_config_t`) and panel description (`epd_panel_def_t`)
are deliberately separate structs: the same panel moved to a different board
has different GPIOs but an identical waveform, and the same board with a
different panel has the same GPIOs but a different waveform. `epd_display.c`
reconciles the two in `board_to_bus_cfg()`
([epd_display.c:1708-1760](src/epd_display.c#L1708-L1760)),
which also resolves a subtlety of 16-bit buses: the LCD peripheral always
emits transfer byte 0 on its own D0-D7 and byte 1 on D8-D15, with no idea
which physical pins the *panel* calls D0-D7 — `bus16_low_byte_first` picks
which half of `epd_board_config_t::data` (given in the *panel's* numbering)
maps to which.

---

## 3. Pixel formats — two different bit depths, two different places

There are **two** pixel encodings in this codebase and they must not be
confused:

1. **Application framebuffer — 4 bits per pixel**, two pixels per byte, high
   nibble = left pixel, value `0x0` = black .. `0xF` = white
   ([epd_fb.h:14-29](include/epd_fb.h#L14-L29)). This is a
   *grey level*, not a drive instruction — it says "how dark this pixel
   should end up looking", nothing about voltages.

2. **Source bus drive code — 2 bits per pixel**, four pixels per byte
   ([epd_display.c:77-106](src/epd_display.c#L77-L106)):

   ```
   bits[7:6] = pixel 0 (leftmost)   bits[5:4] = pixel 1
   bits[3:2] = pixel 2              bits[1:0] = pixel 3 (rightmost)
   ```

   The four possible 2-bit codes are *not* symmetric, and getting the
   distinction wrong is a real (measured) source of image artefacts:

   | Code (ED103TC2) | Name | Electrical meaning | Used for |
   |---|---|---|---|
   | `0b00` | `no_drive` | source line pulled to ground | discharge/settling scans, masked rows |
   | `0b01` | `darken` | drives toward black (VNEG) | GC16/DU "get darker" |
   | `0b10` | `lighten` | drives toward white (VPOS) | GC16/DU "get lighter" |
   | `0b11` | `hold` | source line left alone | unchanged pixels inside a real update |

   `no_drive` and `hold` are **both** "not actively driving toward black or
   white", but they are not interchangeable: with VCOM at its live operating
   voltage, a grounded source line (`no_drive`) still presents a DC field to
   the pixel for the ~15 µs its row is addressed each frame. Over many
   frames — exactly what a partial update, DU idle pass, or the INIT/GC16
   difference matters for — that field measurably darkens what should be an
   untouched region. `hold` genuinely leaves the pixel alone. See the long
   comment at
   [epd_panel_def.h:142-164](include/epd_panel_def.h#L142-L164) and
   the DU idle-bias note in §7.3.

   Which physical code means which direction is a property of the panel's
   source-driver IC and is **not portable** between panel models — it lives
   in `epd_panel_def_t::codes` and must be reverse-engineered/confirmed per
   panel (the ED103TC2 value was cross-checked against a third-party driver
   for the same glass).

Converting from the 4bpp framebuffer format to the 2bpp drive-code format,
frame by frame, *is* waveform generation — that conversion is what the rest
of this document is about.

---

## 4. Bring-up: `epd_panel_power_on()`

Implemented as `epd_display_power_on()`
([epd_display.c:1272-1400](src/epd_display.c#L1272-L1400)).
Order matters because getting rails up in the wrong sequence can latch up the
source or gate drivers:

1. Re-enable source outputs (`OE`) and gate driver mode (`GMOD`) — these were
   left disabled by the previous power-off.
2. `tps65185_wakeup()`, then a fixed 10 ms settle for the PMIC's oscillator.
3. Program the **panel's** rail power-up order into the TPS65185
   (`UPSEQ0`/`UPSEQ1`). This used to be hardcoded once for whichever panel
   the driver was first written against; it now comes from
   `epd_panel_def_t::power` (`epd_power_seq_t`) so each panel can specify its
   own `VDDH`/`VPOS`/`VEE`/`VNEG` strobe assignment and inter-strobe delay
   (3/6/9/12 ms — the TPS65185's only supported steps, rounded down).
4. Program `VCOM` from `epd_panel_def_t::vcom_mv` (see §9 for why this value
   is per-*individual-panel*, not per-model) and call `tps65185_power_up()`.
5. **Poll** power-good rather than sleeping a fixed time: rails come up on
   the PMIC's own sequenced schedule and different rails settle at very
   different times (measured on the reference board: `VPOS`/`VEE`/`VNEG` at
   ~150 ms, `VDDH` not until ~300 ms). A fixed short delay previously reported
   a healthy board as faulty. Times out at 800 ms, polling every 20 ms.
6. Enable the VCOM buffer.
7. Sample the PMIC's thermistor and cache a temperature-derived frame-count
   scale factor for every refresh until the next power cycle (§10).

The panel is now electrically live but shows whatever was last driven onto
it (E Ink is bistable/non-volatile) — nothing has been clocked out yet.

---

## 5. Driving one frame: the row pipeline

Everything about *how* pixels reach the glass — independent of *what*
waveform is running — lives in the "row pipeline" section of
`epd_display.c` ([epd_display.c:391-566](src/epd_display.c#L391-L566)).

### 5.1 Frame start: positioning the gate driver at row 0

`frame_gate_start()`
([epd_display.c:284-339](src/epd_display.c#L284-L339)) runs
once per frame, **inside a critical section** (interrupts disabled): the
gate driver samples `SPV` on a falling edge of `CKV`, so an ISR landing
between two of these GPIO writes stretches a pulse and shifts row 0 by one
line, which shows up as vertical banding or a wrong-looking first row. The
whole sequence costs about 55 µs, which is judged an acceptable amount of
time to hold interrupts off once per frame:

```
CKV↑ → delay(ckv_pre_spv_us) → SPV=0 (assert gate start)
     → delay(spv_low_us) → [spv_sync_lines × CKV pulse, spaced by ckv_extra_us]
     → delay(ckv_post_spv_us) → SPV=1 (deassert)
     → delay(spv_high_us) → CKV pulse → delay → CKV pulse → delay → CKV pulse
     (ends with CKV HIGH — gate now positioned at row 0)
```

`spv_sync_lines` (datasheet **t1**) is how many `CKV` pulses the gate driver
needs to see while `SPV` is held low in order to reliably latch "this is row
0". Most panels in the catalogue need 1; ED115OC1 needs 2 — too short a sync
leaves the gate driver mis-positioned, and the whole image comes out offset
vertically or missing.

### 5.2 Per row: `gate_advance()` + DMA, running one row *ahead*

The gate driver is a shift register — there's no random access, only
"advance to the next row" — so per-row driving is a fixed dance repeated
`height` times. The subtlety is that **the loop runs one row ahead of the
panel**: while row *y*'s pixel data is being shifted into the source driver
over DMA, row *y-1* is the row whose gate is actually open and receiving that
data via `LE`.

`gate_advance()`
([epd_display.c:371-389](src/epd_display.c#L371-L389)), also
inside a critical section:

```
CKV↓                (advance gate shift register to the next row)
LE↑ → LE↓            (latch the PREVIOUS row's already-shifted source data
                       onto that row's pixel lines)
[optional stretch: ckv_low_us]
CKV↑                (re-arm; CKV stays high across the following DMA)
```

then, outside the critical section, the row's 2bpp pixel bytes are DMA'd out
over the source bus while `CKV` is held high. The caller must have already
waited for the *previous* row's DMA to finish before calling `gate_advance()`
— pulsing `LE` while a transfer is still in flight would latch a
half-shifted row.

`row_pipe_submit()`
([epd_display.c:480-542](src/epd_display.c#L480-L542)) is
what actually orchestrates this per row, and it **overlaps CPU work with
DMA**: it waits for row *y-1*'s DMA to finish, paces to the configured row
period if one is set, calls `gate_advance()`, and *then* submits row *y*'s
DMA asynchronously and returns immediately — so the caller can spend the
transfer time building row *y+1* into the other of two alternating row
buffers (`priv->row_buf[0]`/`[1]`). This is why every "build a row" function
in the driver (`build_row_gc16`, `build_row_du`) is
written to run fast enough to hide behind one row's DMA — see §8 for how the
LUTs make that possible.

`row_pipe_pace()`
([epd_display.c:425-452](src/epd_display.c#L425-L452)) is
what makes `epd_ac_timing_t::row_period_us` a real tuning knob rather than
"whatever the build+DMA+scheduler happen to add up to": if non-zero, it
busy-waits (never `vTaskDelay`, which would blow the deadline by handing the
CPU away) until the row's slot in a fixed-period grid comes up, and
re-synchronises rather than trying to claw back time on a later row if one
row overruns. With `EPD_ROW_STATS` enabled, every refresh logs a breakdown
like:

```
71 us/row = build 36 + wait 5 + submit 29 | dma 13, wake 5
```

which is the tool used to choose a `row_period_us` at or above what the
pipeline can actually sustain. On the reference panel (ED103TC2) this is
left at `0` (free-run) — cutting row dwell ~30% across two optimisation
passes produced no visible change, because on that panel tone comes from
*how many phases* drive a pixel (§6), not from *how long* each row is held.

### 5.3 Frame end

`frame_gate_end()`
([epd_display.c:342-353](src/epd_display.c#L342-L353)) latches
the very last row's data (there is no "row N+1" left to trigger it) and
returns `SPV` to its idle-high state. `row_pipe_finish()` additionally
inserts `interframe_us` of settle time before that, and folds the frame's
statistics in.

### 5.4 Partial updates: "no rows are ever skipped"

Because the gate driver is a shift register, a "partial update" cannot mean
skipping rows — every row must still be clocked through to keep row 0 of the
*next* frame correctly positioned. Instead, a partial update means: for rows
or columns **outside** the requested rectangle, send drive codes that leave
the pixel visually untouched.

`epd_rect_t` is expressed in framebuffer coordinates; `window_from_rect()`
([epd_display.c:614-627](src/epd_display.c#L614-L627)) turns
it into an `epd_window_t` of row range + **output byte** column range,
snapping the horizontal bounds *outward* to whole 4-pixel/output-byte
boundaries (the source packing is 4 pixels/byte) and correcting for
`MIRROR_X` (a mirrored panel's column *n* from the framebuffer's left is
column *n* from the panel's right). Rows outside the window are entirely
zeroed/filled with the appropriate "leave alone" code
(`code_fill_byte()`/`mask_row_columns()`,
[epd_display.c:647-661](src/epd_display.c#L647-L661)); within
a partially-masked row, only the out-of-window byte ranges at each end are
overwritten. Rows are still *built* even when they will be discarded, so
that per-row CPU time — and hence drive dwell under the pacing model — stays
uniform across the whole update regardless of where the window edges fall.

---

## 6. The GC16 tone model — how 16 grey levels come from a handful of frames

This is the core "waveform generation" logic and lives in
[epd_display.c:694-881](src/epd_display.c#L694-L881), tuned
per panel in `epd_panel_def_t::wf` (`epd_waveform_def_t`,
[epd_panel_def.h:184-241](include/epd_panel_def.h#L184-L241)).

### 6.1 The physical problem

A raw EPD's ink does not have 16 distinguishable analog voltage-driven grey
levels. On the reference panel it reliably resolves only about **4 temporal
tones** — i.e., about 4 meaningfully different "how many darkening pulses did
this pixel get" states before more pulses stop making a visible difference.
16-level greyscale is therefore *synthesised*, using two orthogonal
mechanisms stacked together:

1. **Temporal dithering** — a GC16 update runs a small, fixed number of
   *phases* (frames); a pixel either "drives" (gets a darkening pulse) or
   doesn't in each phase, and how many phases it drove in encodes darkness.
2. **Spatial dithering** — an 8×8 Bayer matrix perturbs the drive/no-drive
   decision per pixel position, so that *fractions* of a phase's effect can
   be approximated by driving only some pixels in a neighbourhood. This is
   what fills in the "in-between" grey levels a purely temporal 4-phase
   scheme could not reach on its own.

### 6.2 The threshold test

For a pixel at 4bpp grey `level` (0 = black .. 15 = white), in temporal phase
`p`, at framebuffer position `(x, y)`:

```
score  = level_energy[level] + bayer[y & 7][x & 7]
drives = score >= phase_cut[phase_order[p]]
```

`level_energy[]` is the main tone curve — darker levels get more "energy",
nonlinearly spaced (ED103TC2: `64, 50, 38, 34, 30, 26, 22, 19, 16, 13, 10, 8,
6, 4, 2, 0` for levels 0..15). `phase_cut[]` is one threshold per phase,
naturally spaced at `(max_bayer + 1)` intervals — 16/32/48/64 for a
4×4-derived Bayer matrix — because a cut has to *exceed* the largest Bayer
value or that phase would drive some pixels even at zero energy
(this exact invariant is enforced by `epd_panel_def_validate()`,
[epd_panels.c:170-187](src/epd_panels.c#L170-L187)).
`phase_order[]` lets phases run in a different order than they're indexed in,
for panels whose response differs between early and late frames.

The net effect: as `level_energy` rises (darker target), the pixel crosses
more of the phase cuts and therefore drives (darkens) in more of the GC16
phases. Within one phase, the Bayer offset means pixels at the *same* grey
level but different `(x mod 8, y mod 8)` cross the threshold at slightly
different energies — so a grey level that's "70% of the way to the next
darkening phase" is rendered as roughly 70% of pixels in each 8×8 tile
driving that phase, i.e. **dot density**, not partial per-pixel darkening.
This has a real consequence documented in
[epd_panel_def.h:207-211](include/epd_panel_def.h#L207-L211):
until `level_energy` reaches the *first* cut, some pixels never drive at
all and stay pure white — a level at half the first cut's energy is 50%
black dots on white, not a uniform mid-grey, and true solid black needs
energy at or above the first cut.

The Bayer matrix's size *is* the tone-resolution ceiling: a pixel's decision
moves in steps of `1/64` of the matrix's cells, so no curve of
`level_energy`/`phase_cut` can produce finer gradation than that regardless
of tuning. A classic 4×4 matrix (values 0..15) tiled 2×2 into the 8×8 slot
reduces to exactly the same `energy/16` granularity a true 4×4 implementation
would give (and is what every panel tuned before 8×8 support existed still
uses, bit-for-bit); a full 8×8 matrix (values 0..63) is `energy/64` — four
times finer — which was needed on ED115OC1, whose usable energy range is
narrow enough (~1.5 frames to reach black) that 4×4 tone steps collapsed its
highlights onto white.

GC16 assumes the panel starts every update from the fully-white baseline that
`EPD_WAVEFORM_INIT` leaves behind, and each phase can only *darken*
(`no_drive` if not crossing threshold, `darken` if crossing — there is no
`lighten` code in this path at all). This is the path the tone model above
was tuned against, and the one the application uses. `INIT` before every
`GC16` is therefore mandatory, and the ~1.18 s it costs (§7.1) cannot be
avoided — the only way to get a faster update is a `DU` update instead
(§7.3), which does support driving a pixel's actual transition when given
the previous buffer.

---

## 7. The three waveform sequences

`epd_display_refresh()`
([epd_display.c:1626-1674](src/epd_display.c#L1626-L1674))
dispatches a requested `epd_waveform_mode_t` to one of three sequence
functions. Note `EPD_WAVEFORM_GL16` currently aliases `GC16` and
`EPD_WAVEFORM_A2` currently aliases `DU` — there is no separate
implementation for either yet.

### 7.1 `EPD_WAVEFORM_INIT` — full ghost clear

`waveform_init()`
([epd_display.c:1091-1111](src/epd_display.c#L1091-L1111))
drives four alternating full-screen polarity groups — black, white, black,
white — each repeated `init_group_frames` times (temperature-scaled), always
**ending on white** so the panel is at the known baseline the absolute GC16
model requires. Every frame in this sequence is a uniform fill
(`send_frame_uniform`,
[epd_display.c:668-692](src/epd_display.c#L668-L692)) so it
does zero per-pixel work — the buffer content never varies row to row for
a full-screen INIT.

This is deliberately the single most expensive operation the driver
performs: refresh time is *directly proportional to drive energy*
(`height × frames × row_period`), and INIT's job — undoing ghosting from
whatever was on the glass before — needs a measured minimum amount of that
energy. On ED103TC2, `init_group_frames = 2` (8 frames, 416 µs of drive per
pixel) measurably leaves the previous image ghosting through; `4` (16
frames, 832 µs, ~1.18 s wall time) is clean. This makes INIT *drive-limited*
rather than overhead-limited: no software optimisation shortens it without
weakening the clear.

`epd_panel_clean()`
([epd_panel.c:85-115](src/epd_panel.c#L85-L115)) is a
heavier, on-demand tool built on top of this: several full INIT cycles back
to back (3-5 for routine ghosting, up to 10 for a thorough scrub), each
separated by a 50 ms settle — back-to-back cycles are less effective because
the pigment is still moving when the next one starts and never fully reaches
the rail it was being driven to.

### 7.2 `EPD_WAVEFORM_GC16` / `GL16` — 16-level greyscale

`waveform_gc16()`
([epd_display.c:1173-1188](src/epd_display.c#L1173-L1188))
runs `gc16_phases` frames (temperature-scaled by *repeating* each phase,
which preserves the tone mapping rather than adding new ones), each built by
`send_frame_gc16()` using the LUT described in §8.

### 7.3 `EPD_WAVEFORM_DU` / `A2` — fast 2-level black/white

`waveform_du()`
([epd_display.c:1132-1159](src/epd_display.c#L1132-L1159))
is a much simpler, purely binary waveform: no Bayer matrix, no per-level
energy — a pixel is either "dark" (4bpp level below `du_dark_threshold`,
driven `darken`) or "light" (left alone). It runs `du_frames` frames
(rounded up to an even count — see below), each built by `send_frame_du()`.

The one genuinely subtle piece of DU is how "leave alone" is implemented for
the *light* pixels. Neither of the two candidate "don't touch" codes is
actually neutral, and they are not neutral in the *same direction* — measured
on ED103TC2 with a step-wedge of repeated no-op DU passes over identical
white:

```
no_drive (0b00, source grounded) -> the area drifts DARKER over repeated passes
hold     (0b11, source released) -> the area drifts LIGHTER over repeated passes
```

Both effects are invisible for one or two passes and only show up once many
updates accumulate. The fix is the same DC-balancing principle the drive
waveforms themselves rely on: alternate between the two idle codes frame by
frame (`du_build_diff_lut()`,
[epd_display.c:903-947](src/epd_display.c#L903-L947)) so
their opposite biases cancel over a complete update rather than
accumulating — which only works out exactly over a whole number of *pairs*,
hence `waveform_du()` rounding the (possibly temperature-scaled) frame count
up to even.

---

## 8. Turning the tone model into GPIO/DMA traffic: the LUTs

Doing the threshold math from §6 per-pixel, per-frame, in the row build loop
was originally the implementation and cost ~112 µs/row for DU and a full
per-pixel inner loop for GC16 (~10.5 M iterations per GC16 refresh on
ED103TC2) — slow enough that DU ended up *slower* than the INIT+GC16 it's
supposed to be the cheap alternative to. Since row build time has to hide
behind one row's DMA transfer (§5.2), this had to be replaced with
table-driven row construction. Both LUT families follow the same idea:
**everything that is constant for a whole frame gets folded into a table
once, before the row loop starts**, so the per-row inner loop is pure table
lookups with no per-pixel arithmetic.

### 8.1 `gc16_lut` — absolute GC16

Per instance, `priv->gc16_lut[y & 7][parity][half][source_byte]` → a packed
2-bit-pair output nibble
([epd_display.c:180](src/epd_display.c#L180)). The
drive code for a pixel depends only on `(level, phase, x mod 8, y mod 8)`;
phase is fixed for the whole frame, so the table only needs to vary over
`y mod 8` (the Bayer row), which half of an 8-pixel Bayer period an output
byte covers (`parity`, `half` — an output byte holds 4 pixels but the Bayer
period is 8, so two consecutive output bytes cover one period), and the
4bpp source byte's two nibbles. `gc16_build_lut()`
([epd_display.c:760-795](src/epd_display.c#L760-L795)) rebuilds
it (256 × 8 × 2 × 2 entries) only when the requested `(phase, mirror_x)`
differs from what it was last built for — one rebuild per phase per frame,
not per row.
`build_row_gc16()`
([epd_display.c:798-829](src/epd_display.c#L798-L829)) then
walks the row two output bytes (four source bytes) at a time, purely via
table lookups, with the mirrored path walking backwards through the source
row and swapping which table half applies.

For a fully worked, numeric example of this section — real `gc16_level_energy`
and `bayer` values taken through the threshold test cell by cell, up to one
concrete LUT byte — see **[GC16_LUT_VISUAL.md](GC16_LUT_VISUAL.md)**.

### 8.2 `du_diff_lut` — DU

Per instance, `priv->du_diff_lut[parity][(prev_nibble << 4) | next_nibble]` →
a single drive code, built by `du_build_diff_lut()`
([epd_display.c:903-947](src/epd_display.c#L903-L947)). DU has
no Bayer dimension (it's 2-level), so this needs just one flat 256-entry table
per parity, cached against the `dark_threshold` it was built for (see
`priv->du_diff_lut_threshold`); `parity` selects which of the two idle codes
(`no_drive`/`hold`) represents "staying white" in this frame, implementing the
DC-balancing described in §7.3. There is no mirror dimension in the table
itself — `build_row_du()`
([epd_display.c:954-999](src/epd_display.c#L954-L999)) handles
mirroring by walking the source/prev row pointers backwards instead, and does
four table lookups per output byte — one per pixel, since each 2bpp output
byte packs four pixels.

### 8.3 Why these tables live on the panel instance, not at file scope

Both tables are fields of `epd_display_priv_t`
([epd_display.c:166-185](src/epd_display.c#L166-L185)) rather
than `static` file-scope arrays. A single application can legitimately drive
two different panel instances (e.g. two displays on one board); file-scope
tables would let one panel's frame silently corrupt the other's in-flight
LUT the moment both are active, which only became a realistic scenario once
panel definitions became runtime-selectable rather than compile-time
constants.

### 8.4 Mirroring

`EPD_PANEL_FLAG_MIRROR_X`/`_Y` correct for a panel's source/gate drivers
running in the opposite direction to the framebuffer's coordinate system
(several panels in the catalogue need one or the other, and it does **not**
correlate with bus width — an assumption that looked safe on the first four
panels and was refuted by the next three). `MIRROR_Y` is handled simply, by
choosing which framebuffer row (`src_y`) feeds a given panel row `y`.
`MIRROR_X` is handled inside the LUT layer because the Bayer dither is
defined in *source* (framebuffer) coordinates: a mirrored row has to walk
that 8-pixel period backwards, so the table-build functions compute which
Bayer-x each output slot corresponds to under mirroring
(`xb0`/`xb1` in `gc16_build_lut()`) rather than mirroring the *output* after
the fact.

---

## 9. Power-down

`epd_display_power_off()`
([epd_display.c:1460-1590](src/epd_display.c#L1460-L1590))
is a 7-step sequence whose ordering exists entirely to avoid leaving
residual charge unevenly distributed across the panel — the visible symptom
of getting this wrong is a held white image slowly going "grainy" or
speckled after shutdown:

1. Ramp **VCOM to 0 V** (not disabled — disabling floats the common plane,
   which then drifts on leakage and imposes an uncontrolled field on every
   pixel). Settle for `vcom_off_settle_ms`.
2. A short **neutralising scan** of `discharge_frames` uniform `no_drive`
   frames — this *must* run after VCOM is at ground, otherwise the
   `no_drive` code's residual bias against a still-live VCOM darkens/speckles
   the image right as the panel shuts down.
3. **Global discharge**: `gate_all_on()`
   ([epd_display.c:1419-1431](src/epd_display.c#L1419-L1431))
   opens *every* gate row simultaneously (by holding `SPV` low across a full
   frame's worth of `CKV` clocks, filling the gate shift register with
   tokens) so all pixel storage capacitors drain to the grounded source
   lines and grounded VCOM in parallel, for `global_discharge_ms`. This is
   the step that actually removes the per-pixel leftover bias responsible
   for image creep — the per-row neutralising scan in step 2 only connects
   each pixel for its ~15 µs row-active window per frame, far too short to
   bleed a pixel capacitor through the TFT's on-resistance.
4. **Gate flush**: `gate_flush_all_off()`
   ([epd_display.c:1446-1458](src/epd_display.c#L1446-L1458))
   de-asserts `SPV` and clocks `CKV` a full frame's worth again to shift any
   leftover token out of the gate register, isolating every pixel.
5. `tps65185_power_down()` collapses the rails while sources, gates and
   VCOM are all still actively held at a known potential.
6. Release `OE`/`GMOD` — safe now, because the rails are already down and
   releasing them can no longer move charge.
7. `tps65185_standby()`. VCOM is deliberately **left enabled** at 0 V here:
   on this board VCOM has no bleed path to ground other than the PMIC and
   panel themselves, so disabling the buffer would strand its capacitor at
   whatever charge it holds, with the same "drifts on leakage, imposes a
   field on every pixel" failure mode as step 1 — just moved into the (much
   longer) idle period instead of the transition. Actually entering PMIC
   sleep (dropping `WAKEUP`, which kills its I2C interface) is left to the
   application, since that's a whole-system decision.

---

## 10. Temperature compensation

E-ink particle mobility falls sharply in the cold, so a frame count tuned at
room temperature under-drives (washed-out image) below ~15 °C and
over-drives (visible ghost-burn) above ~30 °C. If `epd_panel_def_t::
temp_compensation` is set, `temp_frame_pct()`
([epd_display.c:216-224](src/epd_display.c#L216-L224)) maps a
thermistor reading to a percentage scale applied to every waveform's nominal
frame count via `scale_frames()`:

```
t <  0 °C : 220%      t < 10 °C : 170%      t < 15 °C : 135%
t < 30 °C : 100%  (nominal tuning band)
t < 40 °C :  85%       else      :  75%
```

Sampled once, right after power-on (§4 step 7), and held for the rest of
that power cycle. Readings outside a `[-25, 85] °C` plausibility window are
rejected in favour of the nominal 100% — the underlying
`tps65185_read_temperature()` casts a raw register straight to `int8_t`
without checking conversion completion, so a missing/open/shorted
thermistor can return a railed value like `-128`, which unchecked would
silently more than double every waveform's drive energy.

---

## 11. Panel definitions: the tunable surface

Everything panel-specific — geometry, AC timing, drive codes, and the whole
tone model — is data, not code, living in one `epd_panel_def_t` per panel in
[epd_panels.c](src/epd_panels.c). Adding a new panel means
adding a definition and declaring it in
[epd_panels.h](include/epd_panels.h); nothing in
`epd_display.c` changes. `epd_panel_def_validate()`
([epd_panels.c:133-235](src/epd_panels.c#L133-L235)) checks
the invariants the driver relies on before a definition is used: 8-pixel
width alignment, valid bus width, phase count and ordering, every
`phase_cut` exceeding the Bayer maximum (§6.2), non-zero frame counts, at
least one `spv_sync_lines`, and the four power-rail strobes forming a
genuine permutation of 0..3.

The waveform block (`epd_waveform_def_t`) is explicitly called out as
**empirically tuned per panel and not portable** even to another panel of
the same physical size — several catalogue entries mark themselves
"UNVALIDATED" where the tone model was only inherited from ED103TC2 as a
starting point. The tuning knobs, in the order the source comments suggest
reaching for them, are `gc16_level_energy[16]` (the main tone curve) first,
then `gc16_phase_cut[]` (global darkness bias), then `gc16_phase_order[]`
(for panels whose early/late-frame response differs), then the `bayer[8][8]`
matrix itself (spatial distribution / tone resolution, §6.2). The intended
workflow is to calibrate against greyscale bar/palette test screens,
photograph under constant lighting, identify which levels have visually
collapsed together, and adjust `level_energy` locally around them before
touching anything else.

---

## 12. Summary: one refresh, start to finish

```
epd_panel_refresh_area(panel, NULL, next_buf, EPD_WAVEFORM_GC16, area)
  -> epd_display_refresh()                  resolve area -> epd_window_t
     -> waveform_gc16()                       loop: phase 0..gc16_phases-1
        -> gc16_build_lut()                     fold (phase, mirror) into a LUT — once per phase
        -> send_frame_gc16()                    one full frame:
           -> row_pipe_begin()                    frame_gate_start(): position gate at row 0
           -> for each row y:
                build_row_gc16()                   4bpp -> 2bpp via LUT (build row y+1 ...)
                row_pipe_submit()                  ... while row y's previous DMA is waited on,
                                                    gate advanced, row y's DMA submitted
           -> row_pipe_finish()                    drain last row, settle, restore idle bus state
```

Repeat that whole frame `gc16_phases` (× temperature scale) times, and the
sequence of "some pixels get a darkening pulse this phase, others don't" —
decided per pixel by the energy/Bayer/cut threshold test in §6.2 — is what
the eye integrates into a 16-level greyscale image.
