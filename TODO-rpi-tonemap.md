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
mandatory. Gather-latency-bound like the accurate tier; further CPU vectorisation won't move it.
Faster on letterboxed scope content (fewer luma rows). Offline `dovi_tool` P5→P8.1 remains the
zero-CPU alternative if a box is CPU-starved.

Also related: `frame_is_hdr()` treats HLG (`ARIB_STD_B67`) as HDR but the LUTs are PQ-baked, so
genuine HLG (incl. DV P8.4) is currently mis-tone-mapped (wrong transfer) — bake an HLG curve or
gate it out.

### (original sketch — superseded by the 3D-LUT approach above)
The first guess was `poly-luma 1D-LUT + gather-free NEON-MMR chroma`. We went with a composed 3D
LUT instead: it reuses the validated tetrahedral apply, keeps all DV math scalar/off-hot-path
(unit-testable), and correctly handles luma's cross-channel dependence (a 1D luma LUT can't).

## Accurate-tier perf — where it stands, and a dead end (don't re-try)
The accurate chroma 3D-LUT apply is NEON (8 chroma samples/iter: branchless tetrahedron select +
software gather; `tm3d_chroma_row` in `vf_sand_to_yuv420p_drm.c`), bit-exact to the scalar path.
4K HDR: 0.65× (scalar) → **0.765× (+18%)**. It is **gather-latency bound** — the scattered 4-corner
lookup into the 108 KB LUT is the wall (same wall that beat every V3D offload). Measured ceilings:
a chroma-only 72 KB table repack gave nothing (it's the access *pattern*, not L2 footprint); the
arithmetic-only ceiling (gather stubbed) is ~0.83×.

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
