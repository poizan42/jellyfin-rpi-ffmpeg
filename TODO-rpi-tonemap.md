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

## Dolby Vision profile 5 — NEON RPU-reshaping fast path
Today P5 comes out wrong-coloured: its base layer isn't HDR10, it's Dolby's reshaped IPT-PQ
signal, so the generic PQ tone-map (and plain 10→8 truncation) can't fix it — the correction
lives in the RPU metadata, upstream of tone-mapping. A native fast path is **feasible** and maps
onto the existing machinery; the blocker is correctness effort, not perf or missing inputs.

Building blocks already present: the HEVC decoder runs `ff_dovi_rpu_parse` and attaches
`AV_FRAME_DATA_DOVI_METADATA`; `AVDOVIDataMapping` exposes the reshaping per frame —
`curves[3]` (per component), `mapping_idc` (polynomial vs MMR), `poly_coef[piece][3]`,
`mmr_coef[piece][order][7]`, `num_pivots`. So the coefficients arrive on the CPU for free.

Structure: `poly-luma + MMR-chroma reshape → HDR10 (BT.2020/PQ) → existing tone-map`
(the reshape half is exactly what `dovi_tool` P5→P8.1 does).
- **Luma:** piecewise polynomial (≤9 pivots, order ≤2) → bake a 1024-entry LUT **once per RPU
  update** (RPU changes per scene, not per pixel) and **compose it with the PQ→SDR tone LUT**, so
  the whole luma path stays a single `tbl`+lerp folded into the single-pass unpack (fast-tier style).
- **Chroma:** MMR is a multivariate polynomial in (Y,Cb,Cr), up to order 3 / 7 coeffs — *not* a
  1D LUT (cross-channel, which is why naive P5 chroma is wrong), but **gather-free NEON MACs**
  (evaluate the monomials × coeffs), vectorised across chroma samples, with co-sited luma at
  chroma resolution (reuse the accurate tier's 2×2-average). May be *cheaper* than the 3D-LUT
  (no scattered gather).
- Detect P5 via `dovi_ctx.cfg.dv_profile`; rebuild the composed LUT + reload MMR coeffs on RPU change.

Expected perf: between the fast and accurate tiers (~0.6–0.8× at 4K). **Real cost is correctness:**
DV reshaping is intricate (pivot handling, DV's fixed-point/scaling conventions, MMR monomial
ordering, colour-space bookkeeping) — effectively reimplementing part of `libdovi`'s mapping in
NEON, validated same-frame against `dovi_tool`/libplacebo on CPU. High effort, high risk.

Interim / cheaper options (do these regardless): **detect P5 and warn/skip** so we never silently
emit wrong colour; and note the **offline `dovi_tool` P5→P8.1** route (converts to HDR10 the
pipeline already handles correctly and real-time — zero new code, bit-correct).

Also related: `frame_is_hdr()` treats HLG (`ARIB_STD_B67`) as HDR but the LUTs are PQ-baked, so
genuine HLG (incl. DV P8.4) is currently mis-tone-mapped (wrong transfer) — bake an HLG curve or
gate it out.

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
