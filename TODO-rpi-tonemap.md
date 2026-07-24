# TODO — RPi SAND→YU12 HDR tone-map (vf_sand_to_yuv420p_drm)

Follow-ups deferred while landing the first cut (Hable, zscale-matched tuning, fast + accurate tiers):

## Command-line-tunable calibration
Right now the tone-map reproduces the **zscale + tonemap=hable** tuning as the fixed default
(chosen because it makes highlights read as highlights). Expose the knobs so it can be tuned per
content from the command line, rather than hardcoded:
- `op=` operator select — **add `bt2390`** (ITU-R BT.2390 EETF; the modern "correct" HDR→SDR
  curve, better highlight roll-off) alongside the default `hable`. (reinhard/mobius are cheap adds.)
- `peak=` source peak nits (default: auto from mastering-display / MaxCLL, fallback 1000).
- `target=` SDR target nits (default 100 = REFERENCE_WHITE).
- `desat=` highlight desaturation strength (default 2, matching vf_tonemap).
- `sat=` optional output saturation scale (the manual knob for the "colours vs highlights" trade
  the zscale tuning leans toward highlights on).

Because the LUTs (1D fast, 3D accurate) are built from math at init, these should just re-parameterize
the table builder — no per-frame cost. Rebuild tables only when a knob or the source peak changes.

### Design: build the LUTs at init via libplacebo/zscale; keep the NEON apply  ✅ SHIPPED (zscale, 2026-07-24)
**Shipped for the PQ/HDR10 path** in `vf_sand_to_yuv420p_drm.c`: a `peak=` option (0=auto)
plus runtime detection (MaxCLL→mastering→1000 fallback); when the source peak ≠ 1000 the
PQ LUTs are regenerated at the first frame via an **in-process
`buffersrc→zscale=t=linear:npl=100→tonemap=hable:peak=<nits/100>→zscale=t=bt709→format`
graph** (gated `#if CONFIG_ZSCALE_FILTER && CONFIG_TONEMAP_FILTER`), fed the same synthetic
ramps/grid as `rpi_tonemap_gen.py`; the NEON apply is unchanged. HLG and DV-P5 untouched.
- **Bit-parity verified:** generating at peak=1000 produces byte-identical output to the
  baked tables (framemd5) — confirms the generator reproduces the baked tuning *and* that the
  baked PQ set is 1000-nit.
- **Measured recovery** (same-build A/B, luma-clip ≥234, first frame): Baraka (MaxCLL 1571)
  0.31%→**0.07%**; Exodus (mastering 1200) 2.48%→**1.60%**; slightly darker (correct
  compression). Auto-detected peaks: 1571 / 1200 nits.
- **Fallback:** ~1000-nit and untagged PQ keep the baked tables (no gen); a build **without**
  zscale/tonemap emits a one-time warning and uses the baked 1000-nit tables (`#else` branch,
  compile-verified). libplacebo/`op=bt2390` as an alternative backend remains future work
  (its curve differs from vf_tonemap's hable → not bit-parity).
- **Mid-stream / live streams:** detection + rebuild is per-frame, keyed on `gen_peak`, so a
  peak that changes partway through a stream (spliced / dumped-live) triggers a rebuild at the
  boundary. The HEVC decoder makes the mastering/MaxCLL SEI sticky per coded-video-sequence
  (`hevcdec.c` `set_side_data`), so there is no per-frame thrash. **Caveat (separate, pre-existing
  limitation):** if the peak change coincides with an **SPS change** (different res/framerate at
  the new CVS), the rpivid / v4l2-request HW **decoder** must reconfigure, which currently fails
  on the Pi 4's 512 MB CMA (dma-heap exhaustion → RPS/dst-buffer errors → stall; reproduces with
  `tm=none`, i.e. independent of the tone-map). A pure SEI/peak change with unchanged SPS
  regenerates cleanly. Test clips + write-up: external sample disk `samples/hdr-splice-test/`.

Original design notes (retained):
Today the LUTs are baked at **build time** by `rpi_tonemap_gen.py` shelling out to
`zscale+tonemap=hable` at a fixed 1000-nit peak, embedded as `static const`
(`rpi_tonemap_tables.h`, `RPI_TM_PEAK_NITS 1000`). To make them peak-correct, **move the
generation to filter init** and parameterize by the source peak (from mastering-display /
MaxCLL, or an explicit `peak=`):
- Generate the curve at init from the **real** tone-mapper — **libplacebo** (preferred) or
  **zscale** — via an in-process filtergraph or the lib's C API, then **keep applying it with
  the existing NEON `tbl` path**. Decoupling *generation* (libplacebo/zscale, once per stream)
  from *apply* (NEON, per-frame) means **zero per-frame cost** and **exact fidelity** — no
  reimplementing zimg's curve in C (the reason it was baked externally in the first place).
- Cost is a one-time init table build (~1k luma + ~1k chroma + 33³ grid of simple math /
  a tiny filtergraph pass); rebuild only when the peak or a knob changes. Negligible vs decode.
- **libplacebo folds in three deferred items at once:** peak-awareness, `op=bt2390` (it does
  BT.2390 natively; zscale's `tonemap` doesn't), and it matches Jellyfin's own tonemap backend.
  zscale keeps bit-exact continuity with today's baked `hable` look — could expose both as a
  backend knob. **Same dependency also unblocks the software-decode path's HDR tone-map.**
- The full Jellyfin-FFmpeg build ships **libzimg + libplacebo** already (the lean local config
  omits them only to speed up iteration), so this adds no real dependency. Keep the baked
  1000-nit `rpi_tonemap_tables.h` as an optional no-dep fallback for a stripped build.

**Priority is a quality refinement, not a correctness fix.** Verified (2026-07-24) that the
*current* fixed-1000 pipeline already does something **sensible** on non-DV non-1000-nit HDR10
sources: `tm=fast`/`tm=accurate` both produce correct colour and tone (vs the washed-out
`tm=none`), and the two tiers agree in luma (accurate is slightly more neutral in chroma). The
fixed-1000 assumption shows up only as reduced highlight headroom — above-1000-nit highlights
roll off toward white (hable, graceful; not a hard clip). Mild on ~1200-nit content (Exodus),
visible only in the brightest speculars on 4000-nit masters (Baraka). So peak-aware LUTs are
about **recovering blown highlights on high-peak masters**, not fixing a broken image.

Quantified the recovery (2026-07-24, `zscale=t=linear:npl=100,tonemap=hable:peak=<P>` on the
real files, luma-clip = fraction of Y ≥ 234; peak=10 ≙ 1000 nits reproduces the current baked
curve, confirming units):

| sample (master peak) | clip @ peak=1000 | clip @ correct peak | overall |
|---|---|---|---|
| Baraka (4000) | 0.94% | **0.40%** (peak=40) | ~halves blown highlights; mean 51→47 (slightly darker) |
| Exodus (1200) | 9.9%  | **8.4%** (peak=12) | trims ~1.5 pp; brightest sunlit region keeps texture |

So correct-peak roughly halves the clipped-to-white pixels on a 4000-nit master (and darkens
slightly, the correct compression) — a real but modest highlight-detail gain, confirming the
"quality refinement, not correctness fix" framing. (Measured with a local build after adding
`--enable-libzimg`; `--enable-libplacebo` still pending — its `--enable-vulkan` dependency
fails to configure here despite vulkan 1.3.239 + glslang present, a separate item.)

### Test corpus for the non-1000-nit `peak=` path
The current tone-map assumes a 1000-nit source (the fallback). To validate the auto/`peak=`
path we need HDR10 material authored at a *different* peak. Candidates from Kodi's curated
sample list ([kodi.wiki/view/Samples](https://kodi.wiki/view/Samples)) — referenced by wiki
section + sample title, so they're publicly retrievable — with the authored peak measured via
`ffprobe` (mastering-display `max_luminance` / `MaxCLL`):

**Primary — plain HDR10, HEVC, no Dolby Vision (directly usable by the rpivid pipeline):**
| Kodi section → sample title | mastering peak | MaxCLL |
|---|---|---|
| 4K UHD/HDR → *HDR 10-bit HEVC 24fps (Exodus)* | **1200 nits** | — |
| 4K UHD/HDR → *HDR10+ Profile B HEVC 10-bit 23.976* (Birds of Prey clip) | **4000 nits** | 683 |
| HD audio → *DTS-HD MA 5.1 Baraka HDR Sample* | **4000 nits** | 1571 |

**MaxCLL ≠ 1000 with mastering = 1000 (exercises the MaxCLL branch of auto-peak):**
| Kodi section → sample title | mastering | MaxCLL |
|---|---|---|
| 4K UHD/HDR → *HDR10+ Profile A HEVC 10-bit 23.976* | 1000 | **2279** |
| HD audio → *EAC3-JOC ATMOS Sample* | 1000 | **2666** |

**Non-1000 but Dolby Vision (HEVC; usable only if the DV profile is handled — the pipeline
does DV P5, these are P7/P8):**
| Kodi section → sample title | mastering | MaxCLL / note |
|---|---|---|
| 4K UHD/HDR → *Dolby Vision demos (recovered)* → Food / Landscape / People | 4000 | 10000 / 8507 / 7264 |
| 4K UHD/HDR → *DV FEL vs. BL comparisons* → Days of Thunder HDR12 | 4000 | 2863 (DV FEL, 12-bit dual-layer — likely not rpivid-decodable) |
| 4K UHD/HDR → *HDR vs. Dolby Vision Looped Test Samples* → Alligator | 1000 / MaxCLL 10000 | DV per-frame L1 max = **706 nits** (DV-dynamic non-1000) |

Notes: the transcode pipeline is **HEVC-only**, so the AV1/VP9 HDR samples in the same Kodi
section (e.g. *Costa Rica 4K AV1*, *Alaska 8K AV1*, *HDR10+ VP9*) are out of scope regardless
of peak. Samples with **no** mastering/MaxCLL metadata (e.g. *Camp by Sony*, *iPhone 11 HDR10*)
hit the 1000 fallback and are fallback-path tests, not non-1000 tests. Several entries in the
sample set may still be mid-download — re-probe peaks before relying on them.

## Dolby Vision profile 5 — RPU-reshaping path  ✅ SHIPPED (per-RPU baked 3D LUT + NEON apply)
P5's base layer isn't HDR10 — it's Dolby's reshaped IPT-PQ signal — so the generic PQ tone-map
(and plain 10→8 truncation) came out wrong-coloured; the correction lives in the RPU metadata,
upstream of tone-mapping. **Now handled correctly** in `vf_sand_to_yuv420p_drm.c`.

What we built (differs from the NEON-MMR sketch below, which was the original guess):
- **Detection:** `AV_FRAME_DATA_DOVI_METADATA` present + base layer NOT HDR10-tagged
  (`color_trc != SMPTE2084/ARIB`). Engages regardless of `tm=` (even `tm=none`) so a P5 file
  never passes through wrong-coloured. P8 (keeps SMPTE2084) → existing HDR10 tonemap.
- **Per-RPU bake (scalar C, `p5_bake`):** a 33³ LUT mapping base-YCbCr(full-range 10-bit) → SDR
  8-bit, composing the full DV decode (reshape via `AVDOVIDataMapping.curves` poly/MMR →
  `ycc_to_rgb` → PQ-EOTF → HPE·`rgb_to_lms` → PQ-OETF → BT.2020/PQ HDR10) with the existing
  zscale/hable tone LUT (`ff_rpi_tm_luma1d` + `ff_rpi_tm_lut3d`). Rebuilt only when an FNV hash
  over the consumed RPU coeffs changes (scene cut). All the intricate, silently-wrong-prone DV
  math is confined to ~36K scalar evals off the hot path.
- **Apply (NEON, `p5_luma_row`/`p5_chroma_row`):** fixed-point tetrahedral into the composed LUT,
  extending the accurate tier's `tm3d_calc4` (generalised with a full-range offset). P5 SDR luma
  depends on Cb,Cr (the matrix mixes), so **luma is a full 3D lookup** (full-res Y + nearest
  chroma, gather channel 0) — unlike the HDR10 tier's 1D luma; chroma is block-avg luma + native
  Cb,Cr (gather channels 1,2). Scalar fallback + `SAND_TM_SELFCHECK` (NEON==scalar, max 0).

Validation (bit-close, float GPU oracle — not the bit-exact standard): **Gate A** live-filter
decode→HDR10 vs libplacebo `apply_dolbyvision` = Y 65.1/Cb 62.6/Cr 65.6 dB PSNR, max 1–2 codes;
**Gate B** composed SDR (bake+tetra apply) vs oracle-HDR10→accurate-tonemap = 51–54 dB, max 2–3
codes; visual on real scenes correct (skin tones, night ambiance, no cast). MMR cross-channel math
validated by a standalone unit test vs a literal transcription of libplacebo's GLSL reshape
(the sole local P5 file, Agatha S01E05, is trivial poly-only).

Perf (full 4K→720p, steady state): scalar **0.145×** → NEON **0.555×** (3.8×). As expected,
between... well, near the accurate tier (0.765×) but heavier because the luma 3D lookup is
mandatory. **(All figures in this paragraph predate `TM_CHUNK`=4 — see the "Accurate-tier perf"
section below; that L1-tiling win later raised P5 default 0.555→~0.60× and accurate 0.765→~0.94×.)**
Gather-latency-bound like the accurate tier; further CPU vectorisation won't move it.
Faster on letterboxed scope content (fewer luma rows). Offline `dovi_tool` P5→P8.1 remains the
zero-CPU alternative if a box is CPU-starved.

**Fast tier (`tm=fast`) ✅ SHIPPED.** P5 now honours the `tm=` knob: default/`tm=accurate` = full
3D luma+chroma (above); `tm=fast` (TM_P5_FAST) approximates luma with a **1D neutral-chroma curve**
(`p5_luma1d`, baked per-RPU alongside the 3D LUT via the same `p5_decode_hdr10`+tone-curve, Cb=Cr=mid)
applied with `lut1d_apply`, dropping the ~8.3M/frame 3D luma lookups; chroma stays the full 3D path
(it's genuinely cross-channel). Structurally identical to the HDR10 accurate tier (10-bit scratch +
1D luma + 3D chroma). **~0.73× at 4K** (vs 0.55×) — since raised to **~1.0× (real-time)** by
`TM_CHUNK`=4 (see below), and `tm=veryfast` adds ~6% more (nearest chroma); luma ~45–51 dB vs the 3D
path on real content (chroma bit-identical). Default P5 output is byte-unchanged (md5). Note: the single-pass `y8_lut`
kernel is deliberately NOT used for P5 luma — the 3D chroma pass needs the 10-bit luma for 2×2
co-siting anyway, so single-pass luma would force the slower "2-read" variant (see the dead-end note
above); reusing the existing scratch + 1D `lut1d_apply` is both simpler and faster.

**Dead end — by-calculation apply (do not re-attempt).** Tested replacing the 3D-LUT gather with
per-pixel arithmetic: fused DV-decode + `vf_tonemap=hable`/desat + BT.2020→709 gamut + BT.709 OETF +
RGB→YCbCr709, with PQ EOTF / BT.709 OETF as small L1-resident 1D transfer LUTs instead of `pow`
(the "trade the scattered 108 KB gather for cache-friendly compute" idea; standalone bench
`scratchpad/dv_calc_bench.c`). The fusion is valid — the DV decode ends in linear BT.2020 light and
the tonemap re-linearizes, so the PQ-OETF→YCbCr→RGB→PQ-EOTF round-trip cancels to ×100. But **it
loses decisively on perf**: scalar 4K, DRAM-resident, vs the shipped 3D tetrahedral gather (1.00×):
exact-`pow` calc 0.05×, transfer-LUT calc 0.13×, and — the decisive number — **arithmetic-only
ceiling (transcendentals free) 0.17× (≈6× slower)**. The ~5 3×3 matrix-MACs + hable + desat per
pixel are inherently far more work than one 4-corner gather + weighted sum; NEON helps the calc's
arithmetic more than the (poorly-vectorising) gather but cannot flip a 6× deficit that exists before
the transfer-LUT gathers are even added back. Confirms empirically that the A72 4-corner gather into
the 108 KB LUT is cheap in absolute terms and precomputation wins. (Correctness of the calc path was
not fully pinned down — it diverges from the shipped LUT partly by luma *model*: the composed LUT's Y
is a 1D tone curve on the reconstructed Yh, the calc does full RGB hable — but that's moot given the
perf loss.) The real pipeline bound remains DRAM bandwidth (whole-frame re-reads), which the calc
path shares and does not relieve.

### HLG (ARIB_STD_B67) transfer  ✅ SHIPPED
Was: `frame_is_hdr()` accepted HLG but the LUTs were PQ-baked, so genuine HLG (incl. DV P8.4) was
mis-tone-mapped through the PQ curve. Fixed by baking a **second, HLG-input LUT set**
(`ff_rpi_tm_*_hlg`, `rpi_tonemap_gen.py` now emits both PQ and HLG in one run — identical hable
chain/grid, only the input `-color_trc` differs) and selecting by the frame's `color_trc` in
`filter_frame` (thread active table pointers through `TMData`; `frame_is_hdr()` and the P5 gate
unchanged, so P5/P8.1/P8.4 separation is intact). Validated: PQ output byte-identical (md5),
`checkasm` + `SAND_TM_SELFCHECK` clean, HLG vs its zscale oracle 41/49/54 dB (Y/Cb/Cr — same
fidelity the PQ tier gets), and ~17.7/255 mean-RGB correction vs the old PQ-on-HLG path, on a real
main10 HLG clip. Note: **DV P8.4** (HLG base) tone-maps its HLG base directly and ignores the RPU
for v1 — consistent with how P8.1 handles its HDR10 base.

### (original sketch — superseded by the 3D-LUT approach above)
The first guess was `poly-luma 1D-LUT + gather-free NEON-MMR chroma`. We went with a composed 3D
LUT instead: it reuses the validated tetrahedral apply, keeps all DV math scalar/off-hot-path
(unit-testable), and correctly handles luma's cross-channel dependence (a 1D luma LUT can't).

## `out=half` — fused 2×2 downscale for exact 4K→1080p — SHIPPED (big win)
For a 2:1 downscale the filter emits 1080p directly (`out=half`): box-average 2×2 right after the SAND
unpack (`box2x2_row`, NEON `vpaddq_u16`+`vrshrq_n_u16`, bit-exact vs scalar), run the **existing** apply
at 1920×1080 / 960×540 into a half-res dst, and hand it straight to the encoder — **dropping the
`scale_v4l2m2m` ISP stage**. Reuses `p5_apply_chunk`/`tm_apply_chunk` verbatim (they're dim-parameterized);
the only new code is the box-average + a half branch in `tm_slice` + `out` option/plumbing. Co-siting is
automatic (chroma `avgY4` becomes 2×2 of half-res luma = 4×4 of 4K). All P5 tiers use **tetrahedral chroma
at half-res** (4× fewer sites → cheaper than the full-res nn path *and* better; `fast`/`veryfast` collapse
to it, nn/dither unused on this path). **Measured 4K→1080p (single-tenant, 500-frame): DV P5 accurate
0.66→1.12×, DV P5 fast/veryfast 0.90–0.93→1.19–1.25×, HDR10 accurate 0.89→1.26×** — all cross real-time.
Two mechanisms, both real: the apply shrinks ~4×, *and* removing the ISP's ~15.5 MB/frame bus traffic
relieves DRAM contention on the memory-latency-bound unpack (same filter: ~27 ms alone vs ~40 ms
in-pipeline). Quality: ordering error (down-then-map vs map-then-down) negligible at 57–60 dB / max ~2
codes (the order libplacebo/mpv use); box 2×2 vs ISP polyphase = slight softening. Deterministic,
NEON==scalar; `out=full` (default) byte-identical. **So at 4K→1080p even DV P5 `tm=accurate` is real-time —
the `dovi_tool` P5→P8.1 offline fallback is no longer needed for this ratio.** Note the early SDR NEON-scale
concern didn't apply: that was a *standalone* pass with a free apply + free ISP; here the average is fused
into an expensive apply and the ISP was *contending*, not free. Only helps exact 2:1 (the 4K→720p 3:1 case
stays full-res + ISP, keeping the `veryfast`+dither tier). Follow-up: box→[1,3,3,1] tap if softening matters.

## Accurate-tier perf — where it stands, and a dead end (don't re-try)
The accurate chroma 3D-LUT apply is NEON (8 chroma samples/iter: branchless tetrahedron select +
software gather; `tm3d_chroma_row` in `vf_sand_to_yuv420p_drm.c`), bit-exact to the scalar path.
4K HDR: 0.65× (scalar) → **0.765× (+18%)**. A chroma-only 72 KB table repack gave nothing.

**`TM_CHUNK`=4 tiling — SHIPPED, big cheap win (bit-exact).** The 10-bit scratch is tiled in
`TM_CHUNK`-row bands; it was 16, which thrashes L2 at 4K. Swept 2/4/8/16 (SAND_PROF, Echo accurate):
16 ≈ 44 ms/frame, 8 ≈ 38.6, **4 ≈ 37.5**, 2 ≈ 37.5 — 4 is the knee (L1-resident, past which loop
overhead offsets the smaller footprint). End-to-end +13–18% across all scratch tiers, **bit-identical
output** (pure tiling granularity): HDR10 fast 1.11→**1.17×**, HDR10 accurate 0.77→**~0.94×**, DV-P5
default 0.55→**0.60×**, **DV-P5 `tm=fast` 0.73→~1.0× (real-time)**. This recovered ~6–7 ms of the
~8 ms "lost-fusion" cost below, cheaply — reframing the register-fused kernel (next para) as marginal.

**Register-fused SAND→apply kernel — TRIED, NEGATIVE (do not re-attempt).** Built the fused
SAND30-unpack + 1D-luma + 3D-chroma → YU12 kernel: per 128-byte stripe (96 luma / 48 chroma sites),
transcribed `USAND10` into tiny natural-order stack tiles (`vst3q`; chroma splits via `vld2q` since
the 3-per-word stream is `U0,V0,U1,V1,…`), applied inline with all NEON constants hoisted out of the
stripe loop (reusing `tm3d_calc4`), never writing a full 10-bit plane. Bit-identical to the
`s10`-scratch path (md5). Both a reuse variant (per-stripe `lut1d_apply`/`tm3d_chroma_row` calls) and
the fully-inlined variant were **no faster than `TM_CHUNK`=4** — SAND_PROF (Echo accurate): scratch
36.9 ms vs fused 37.5 ms (marginally *slower*); end-to-end within run noise.
**Why it failed:** the ~3.5 ms residual vs the fast tier is *not* the intermediate write (which
`TM_CHUNK`=4 already made L1-cheap) — it's the **inherent 10-bit luma unpack** the cross-channel
chroma 2×2 co-siting requires. The fast tier's `y8_lut` narrows to 8-bit *inside* the unpack and never
forms 10-bit luma; the accurate/P5 chroma *must* have 10-bit luma, so that unpack cost is unavoidable
regardless of fusion. Fusing only removes the (already-cheap) intermediate store, not the unpack.
`TM_CHUNK`=4 captured essentially all the recoverable perf; P5-`fast` at ~1.0× is the real-time result.

**Nearest-neighbour 3D chroma — SHIPPED as opt-in `tm=veryfast` (P5 only).** Replaces the tetrahedral
chroma apply (4-corner gather + 4-tap Q8 wsum + tetra select) with a nearest-grid-cell lookup: round
`(avgY4,Cb,Cr)` to one cell → **1 gather, no select, no wsum** (`p5_chroma_nn_row`/`_scalar`). NEON ==
scalar (SAND_TM_SELFCHECK max 0); the tetrahedral paths stay bit-identical (veryfast is a separate
branch). **Gain +~6%** — P5-`fast` ~1.0→~1.02–1.05× (SAND_PROF setup+unpack ~37.5→~34.5 ms). The chroma
apply is only ¼-resolution (4:2:0), so halving it saves ~3 ms of a ~37 ms filter, not ~5.
**Quality is coarser and this was initially rejected**, then kept as an explicit opt-in tier because
P5-`fast` at ~1.0× has no headroom for concurrent load (background work → playback stutter), and +6%
buys that headroom. Cost: on HDR10-style smooth gradients nn-vs-tetra chroma is only ~38 dB (max 15–16
codes; the lake shot shows pink/red blotching from cells snapping across the 33³ grid's ~28-code
spacing); P5 real content (Agatha) is milder (~46–48 dB, max 7–8). So it's **P5-only and opt-in**:
HDR10/HLG `tm=veryfast` falls through to `tm=fast` (their chroma is already the cheap separable path —
nothing to drop), and `fast`/`accurate` remain the defaults. Wiring: `TM_VERYFAST`→`TM_P5_VERYFAST`
in `filter_frame`; `p5_apply_chunk` uses the 1D luma (like fast) + `p5_chroma_nn_row`. A linear-in-Y +
nearest-chroma middle ground would cost most of the arithmetic back for a fraction of the gain — not
pursued. The tetrahedral arithmetic remains the floor for the accurate-quality tier.

**De-banding `veryfast` — SHIPPED (ordered Bayer dither, ~free).** The nearest lookup's one flaw was
grid-snap banding on smooth gradients. Fixed by **dithering the LUT coordinate** before the round: the
`(q+128)>>8` nearest-round's `+128` becomes a per-pixel `BAYER8[...]*4+2` bias (full amplitude, spanning
the whole [0,256) interval), decorrelated per axis (tile phase Y(0,0)/U(+3,+5)/V(+6,+2)), so cell
boundaries dissolve into sub-visible grain that averages back to the interpolated colour. `p5_chroma_nn_row`
builds 3 per-row bias vectors once (x%8==0 in the 8-wide loop ⇒ lane→column is a fixed per-row pattern),
so the NEON inner loop is byte-for-byte the same op count as plain nearest — **the +6% is fully retained
(~1.06×)**. Deterministic (function of x,y → md5-reproducible), NEON==scalar bit-exact (`SAND_TM_SELFCHECK`
max 0). Quality (real P5, Agatha): per-pixel chroma ~45–51 dB vs tetrahedral (grain), **perceptual
(5×5-blurred) ~63 dB** — visually the blotches are gone. This is what a research-agent investigation +
standalone prototype (`scratchpad/dither_bench.c`: nearest 40–45 dB → dither 53 dB blur-PSNR, +10–14 dB)
recommended over the alternatives (bilinear-in-chroma / single-axis-linear re-add gathers+arithmetic →
collapse toward `fast`; finer 65³ grid keeps one gather but bloats the LUT 108KB→824KB out of cache).
Follow-ups if ever needed: `amp128` bias (`*2+1`) halves the grain for a chroma-bitrate tradeoff; a 64×64
blue-noise tile is a same-cost drop-in perceptual upgrade over Bayer.

**Profiled breakdown (SAND_PROF, Echo 4K, filter unpack+apply wall time), correcting the earlier
"gather is the wall" framing** — it isn't. (Numbers below are the *pre-`TM_CHUNK`=4* wall times that
motivated the fusion work — `TM_CHUNK`=4 later cut accurate to ~37.5 ms; the *split* still holds.)
`tm=fast` = 34 ms/frame; `tm=accurate` = 51 ms/frame.
The +17 ms accurate penalty splits as: **~8 ms = loss of single-pass luma fusion** (the 3D chroma
needs co-located 10-bit Y+CbCr, forcing a full 10-bit intermediate + a separate luma pass instead
of the fused SAND→8-bit single pass — ~2.7× luma traffic); **~9 ms = the 3D chroma apply**, which is
almost entirely *fixed-point arithmetic* (coord map + branchless tetra select + 4-tap weighted sum);
and only **~1.6 ms = the actual scattered gather into the 108 KB LUT** (measured by stubbing it:
51→~50 ms). So the big LUT's random access is ~10% of the gap — the real costs are the lost fusion
(memory traffic) and the apply *compute*. (Consistent with the by-calc dead-end above: on the A72
the tetrahedral gather is cheap; arithmetic is what's expensive.)

**Dead end — do not re-attempt:** fusing the luma tone-map into the SAND unpack (a `y16`+`y8lut`
two-output `.S` kernel, so luma skips the separate `lut1d` pass). Built and validated bit-exact,
it gave **~0% (0.765→0.774×, within run noise)**. Reason: the "15% luma-output" cost is almost
entirely the *mandatory* 8-bit output write; the `lut1d` re-read it removed was already L2-hot/cheap.
The 2-read variant (single-pass luma + separate `y16`) was *slower* (0.718×) — the extra scattered
SAND read costs more than the pass it saves. Bottom line: the accurate tier is memory/gather bound;
further CPU-side vectorisation won't move it. (A GPU 3D-LUT would suit the gather but libplacebo
doesn't run on the Pi's V3DV, and the DRAM round-trip loses on unified memory — see the fork README.)

## Other
- 32-bit `arm/rpi_sand_neon.S` parity for the fast-tier tone LUT (aarch64 done first; Pi 4 is 64-bit).
