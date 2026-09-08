# From `gc16_level_energy` + `bayer` to a GC16 LUT byte — a visual walkthrough

This is a worked-example companion to [EINK_DETAILS.md](EINK_DETAILS.md),
zoomed all the way in on one question: **given a pixel's 4bpp grey level and
its position on the panel, how does `gc16_build_lut()` decide the 2-bit drive
code that ends up sitting in `priv->gc16_lut[...]`?**

All numbers below are the real, shipped tuning for **ED103TC2**
([epd_panels.c:102-130](src/epd_panels.c#L102-L130)) — nothing
here is illustrative-only.

---

## 1. The three ingredients

**`gc16_level_energy[16]`** — one "darkness score" per 4bpp grey level. Level
0 is black, level 15 is white, and energy falls as level rises:

```
level  :  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
energy : 64  50  38  34  30  26  22  19  16  13  10   8   6   4   2   0
         └──────────────── darker ────────────┴──────── lighter ────────┘
```

**`bayer[8][8]`** — a fixed per-pixel-position offset, tiled from a classic
4×4 ordered-dither matrix so each of the 16 distinct values (0..15) appears
**exactly 4 times** across the 64 cells:

```
        x=0  x=1  x=2  x=3  x=4  x=5  x=6  x=7
y=0 →    0    8    2   10    0    8    2   10
y=1 →   12    4   14    6   12    4   14    6
y=2 →    3   11    1    9    3   11    1    9
y=3 →   15    7   13    5   15    7   13    5
y=4 →    0    8    2   10    0    8    2   10     (rows 4-7 repeat 0-3 —
y=5 →   12    4   14    6   12    4   14    6       the matrix has an 8-row
y=6 →    3   11    1    9    3   11    1    9       PERIOD but only 4 rows
y=7 →   15    7   13    5   15    7   13    5       of distinct content)
```

**`gc16_phase_cut[4]`** = `{16, 32, 48, 64}` — one threshold per GC16 frame
(`gc16_phase_order` is the identity `{0,1,2,3}` on this panel, so phase *p*
uses `phase_cut[p]` directly).

## 2. The test, as a picture

[epd_display.c:755-757](src/epd_display.c#L755-L757):

```c
score = level_energy[level] + bayer[y & 7][x & 7];
code  = (score >= phase_cut[phase]) ? darken : no_drive;
```

Think of `level_energy` as how full a bucket already is, `bayer` as a
per-cell bump that varies the rim height by up to 15 units, and
`phase_cut` as the water level being raised on each successive frame. A
pixel "spills" (drives `darken`) on every phase whose cut it has enough
combined energy + bump to clear:

```
score = energy + bayer(x,y)
             ▲
      64 ────┼───────────────────────────  phase 3 cut
             │        ██
      48 ────┼────────██───────────────    phase 2 cut
             │  ██     ██
      32 ────┼──██─────██────────────      phase 1 cut
             │  ██  ██  ██
      16 ────┼──██──██──██───██──────      phase 0 cut
             │  ██  ██  ██   ██  ██
             └──────────────────────────▶
              lvl0 lvl2 lvl6 lvl9 lvl15
             (bar height = energy; bayer adds ±0..15 jitter on top)
```

## 3. Worked example — level 6 (energy 22), all 64 cells

`score(x,y) = 22 + bayer[y][x]`. Below is that full 8×8 score grid, then
which cells clear each of the four cuts.

```
scores (22 + bayer):
        x=0  x=1  x=2  x=3  x=4  x=5  x=6  x=7
y=0 →   22   30   24   32   22   30   24   32
y=1 →   34   26   36   28   34   26   36   28
y=2 →   25   33   23   31   25   33   23   31
y=3 →   37   29   35   27   37   29   35   27
y=4 →   22   30   24   32   22   30   24   32
y=5 →   34   26   36   28   34   26   36   28
y=6 →   25   33   23   31   25   33   23   31
y=7 →   37   29   35   27   37   29   35   27
```

**Phase 0, cut = 16.** Every score above is ≥ 16 (minimum possible score is
`22 + 0 = 22`), so **every one of the 64 cells drives**. Level 6 gets a full,
uniform `darken` pulse in phase 0 — no dithering visible yet:

```
phase 0 drive mask (# = darken, . = no_drive):
########
########
########
########
########
########
########
########          64/64 cells drive  (100%)
```

**Phase 1, cut = 32.** Now only cells with `bayer ≥ 10` clear (`22+10=32`).
Scanning the bayer grid for values 10, 11, 12, 13, 14, 15:

```
phase 1 drive mask:
...#...#
#.#.#.#.
.#...#..
#.#.#.#.
...#...#
#.#.#.#.
.#...#..
#.#.#.#.        24/64 cells drive  (37.5%)
```

**Phase 2 (cut 48) and phase 3 (cut 64):** need `bayer ≥ 26` / `≥ 42`, but
the matrix tops out at 15, so **no cell ever clears these** for this level —
both masks are all `.`.

So level 6 ends up with **one uniform frame of drive (phase 0) plus a
dithered 37.5%-density second frame (phase 1)** — the panel gets driven
harder than a flat "1 frame" grey but nowhere near the 4-frame black.

## 4. Worked example — level 11 (energy 8), pure dot-density

This is the case called out in
[EINK_DETAILS.md 6.2](EINK_DETAILS.md#62-the-threshold-test): energy below
the first cut, so the *whole* grey level is expressed as dot density in a
single phase, with no uniform component at all.

`score(x,y) = 8 + bayer[y][x]`. Phase 0 (cut 16) needs `bayer ≥ 8`:

```
phase 0 drive mask, level 11 (energy 8):
.#.#.#.#
#.#.#.#.
.#.#.#.#
#.#.#.#.
.#.#.#.#
#.#.#.#.
.#.#.#.#
#.#.#.#.        32/64 cells drive  (50.0%)
```

Phases 1-3 need `bayer ≥ 24/40/56` — impossible — so those masks are empty.
**This grey level is *entirely* a 50%-density stipple of black dots on
white**, driven for exactly one frame — and at this particular threshold the
mask happens to land on a perfect checkerboard. Zoom into any 8×8 tile of a
level-11 fill and you will see literal alternating-pixel speckle, not a
smooth grey — this is the physical reason the driver's own docs insist grey
is "dot density, not partially darkened pixels."

## 5. The full 16-level × 4-phase drive-fraction table

Because every one of the 16 distinct Bayer values occupies exactly 4 of the
64 cells, the fraction of cells clearing a cut has a closed form —
`k = cut − energy` cells worth of headroom is needed, and each unit of `k`
removes exactly `4/64 = 1/16` of the cells:

```
drive_fraction(energy, cut) = clamp( (16 − cut + energy) / 16,  0,  1 )
```

Applying that with `cut ∈ {16, 32, 48, 64}` to every level gives the whole
tone ramp at a glance:

```
lvl energy  phase0(c16)  phase1(c32)  phase2(c48)  phase3(c64)   picture
 0    64      100%         100%         100%         100%        ████
 1    50      100%         100%         100%          12.5%      ███▁
 2    38      100%         100%          37.5%          0%       ██▁_
 3    34      100%         100%          12.5%          0%       ██▁_
 4    30      100%          87.5%          0%           0%       █▇__
 5    26      100%          62.5%          0%           0%       █▆__
 6    22      100%          37.5%          0%           0%       █▃__   ← 3
 7    19      100%          18.75%         0%           0%       █▂__
 8    16      100%           0%            0%           0%       █___
 9    13       81.25%        0%            0%           0%       ▇___
10    10       62.5%         0%            0%           0%       ▆___
11     8       50%           0%            0%           0%       ▅___   ← 4
12     6       37.5%         0%            0%           0%       ▃___
13     4       25%           0%            0%           0%       ▂___
14     2       12.5%         0%            0%           0%       ▁___
15     0        0%           0%            0%           0%       ____
```

(`█`/`▇`/`▆`/…/`▁` are just a rough bar for that phase's fill fraction, `_`
means the phase never fires for that level.) Reading down the table is
reading the whole design in one place:

* **Level 8 is the hinge.** Energy 16 exactly equals `phase_cut[0]`, so it
  drives phase 0 uniformly (100%, no dithering) and nothing else — the
  lightest level that still gets a full, undithered frame of drive.
* **Levels 9-15** (below the first cut) are *pure* single-phase dot density,
  exactly like 4 — this is the only mechanism producing the panel's
  lightest greys, which is why they collapse together first if a panel's
  usable energy range is too narrow (the ED115OC1 case noted in
  [epd_panel_def.h:207-211](include/epd_panel_def.h#L207-L211)).
* **Levels 0-7** (above the first cut) always drive phase 0 at 100% and
  layer dithered fractions of phases 1-3 on top — darkening is "add another
  mostly-full frame", not "increase the dot density further" once phase 0
  saturates.
* Only **level 0** ever reaches 100% in phase 3 — that's the panel's true
  black, four full frames of `darken`.

## 6. From a drive mask to an actual LUT byte

`gc16_build_lut()` doesn't store per-pixel masks like the ones above — it
pre-combines every *pair* of pixels sharing one 4bpp source byte into a
single LUT entry, indexed by the raw byte value, so the row-build loop never
touches individual pixels at runtime. The struct is
`gc16_lut[y & 7][par][half][256]`
([epd_display.c:180](src/epd_display.c#L180)), and the
`(par, half)` pair selects which 2 of the 8 pixels in one Bayer period
(`x mod 8`) a given table slot covers:

```
one Bayer period (8 pixels, x = 0..7) along a row:
 source bytes:     src[0]        src[1]        src[2]        src[3]
                  ┌─────┴─────┐┌─────┴─────┐┌─────┴─────┐┌─────┴─────┐
 pixel (bayer x):   x=0   x=1    x=2   x=3    x=4   x=5    x=6   x=7
 LUT slot:        par0,h0     par0,h1      par1,h0      par1,h1
                  xb0=0,xb1=1 xb0=2,xb1=3  xb0=4,xb1=5  xb0=6,xb1=7

 output bytes:     row_buf[b]      row_buf[b]      row_buf[b+1]    row_buf[b+1]
                   hi nibble       lo nibble        hi nibble       lo nibble
```

i.e. **one Bayer period = 4 source bytes = 2 output bytes**, and each LUT
slot's job is: given a source byte's two 4bpp nibbles, and knowing which two
fixed `x` positions (`xb0`, `xb1`) and which Bayer row (`y & 7`) they sit at,
precompute the packed 2-bit-pair code for *both* pixels at once.

### Concrete cell: `gc16_lut[0][0][0][0x6B]`, phase 0 vs phase 1

Take `yb = 0` (top Bayer row), `par = 0, half = 0` → `xb0 = 0, xb1 = 1`
(the leftmost pair in a period), and source byte `0x6B` — hi nibble `0x6`
(level 6, energy 22, from 3), lo nibble `0xB` = 11 (level 11, energy 8,
from 4).

**Phase 0 (cut 16):**

```
pixel 0: level 6,  xb=0, yb=0 → bayer[0][0] = 0   → score = 22+0 = 22 ≥ 16 → darken (0b01)
pixel 1: level 11, xb=1, yb=0 → bayer[0][1] = 8   → score =  8+8 = 16 ≥ 16 → darken (0b01)

packed nibble = (c0 << 2) | c1 = (0b01 << 2) | 0b01 = 0b0101 = 0x5

  gc16_lut[0][0][0][0x6B]  (phase 0)  =  0x5
```

**Phase 1 (cut 32):**

```
pixel 0: level 6,  score 22 < 32 → no_drive (0b00)
pixel 1: level 11, score 16 < 32 → no_drive (0b00)

packed nibble = (0b00 << 2) | 0b00 = 0x0

  gc16_lut[0][0][0][0x6B]  (phase 1)  =  0x0
```

This is exactly what 3/4's masks predicted: pixel 0 (level 6) drives in
both phase 0 and phase 1 at this position (bayer 0 is small, so it clears
both cuts); pixel 1 (level 11) drives only in phase 0. The whole 256-entry
table for `[yb=0][par=0][half=0]` is just this same three-line calculation
repeated for every possible byte value, and the whole 8×2×2×256 table is
that repeated again for every `(yb, par, half)` combination — 8192 cells
total, rebuilt once per phase
([epd_display.c:762](src/epd_display.c#L762): the rebuild is
skipped entirely if `(phase, mirror_x)` hasn't changed since the last row).

### Assembling a whole output byte

`build_row_gc16()` ([epd_display.c:798-829](src/epd_display.c#L798-L829))
then just chains four such lookups per Bayer period:

```
src row:     [ 0x6B ][ .. ][ .. ][ .. ]   (4 source bytes = 8 pixels)
              │
              ├─ lut[0][0][0x6B]  = 0x5  ┐
              ├─ lut[0][1][src1]  = 0xZ  ┘─▶ row_buf[b]   = 0x5Z   (pixels x0..x3)
              ├─ lut[1][0][src2]  = 0xY  ┐
              └─ lut[1][1][src3]  = 0xW  ┘─▶ row_buf[b+1] = 0xYW   (pixels x4..x7)
```

which reproduces the byte layout documented at
[epd_display.c:80-84](src/epd_display.c#L80-L84): bits
`[7:6]` = pixel 0's 2-bit code, `[5:4]` = pixel 1's, `[3:2]` = pixel 2's,
`[1:0]` = pixel 3's — now built from two table lookups instead of four
per-pixel threshold evaluations.

## 7. Why this is worth precomputing

Per row, `build_row_gc16()` does **2 table lookups per output byte** and no
arithmetic at all — no threshold compare, no array indexing into
`level_energy`/`bayer`, no branch. The cost of the energy/bayer/cut math in
2 is paid **once per phase** (rebuilding ≤ 8192 table cells), not once per
pixel per row per phase. This is the difference the code comment at
[epd_display.c:744-745](src/epd_display.c#L744-L745)
measures directly: replacing a `width`-iteration per-pixel loop (~10.5M
iterations across a full ED103TC2 GC16 refresh) with a `width/4`-iteration
table-driven loop, which is what lets the row build stay inside one row's
DMA transfer time (5.2 of EINK_DETAILS.md) instead of stalling the
pipeline.
