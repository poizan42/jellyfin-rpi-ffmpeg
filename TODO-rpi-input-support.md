# TODO — RPi input-format coverage (widen what the transcode path accepts)

Context: the current pipeline is HEVC **Main/Main10, 4:2:0 8/10-bit, ≤4K** (rpivid decode +
4:2:0-only SAND→YU12 unpack + 8-bit H.264 encode). The supported-input matrix and the empirical
basis are in `README.jellyfin-rpi.md` (§Supported input). Validated against the ByteDance HEVC demo
corpus (`../samples/ByteDance-HEVC/`, 59 clips; per-clip `hw_transcode` verdicts in `videos.json`):
**37 transcode, 22 don't.** The 22 split into two groups, each a work item below.

Key established fact: the decoder gate is **pixel format, not profile string** — `Range Extensions`
(RExt)-tagged streams that are actually 4:2:0 8/10-bit already decode + transcode fine. So the gaps
below are genuinely about chroma/bit-depth and resolution, not the profile name.

---

## 1. `FAIL-filter` — non-4:2:0 / >10-bit HEVC (want: handle all of them)

**Goal: support all of these.** Whether each can be *real-time* is an open question we'll answer as we
build the kernel/filter support — correctness first, perf measured per-format as it lands.

These decode (or would decode) but the SAND→YU12 unpack can't ingest the pixel format — it's 4:2:0
8/10-bit only. Sixteen corpus clips, spanning:
- **4:2:2** (`yuv422p` / `yuv422p10le` / `yuv422p12le`): `hevc_4k60P_rext_10bit_422_eos_r5c_1`,
  `hevc_4k50P`, `quick-brown-fox-…-rext-{8,10,12}bit-422`
- **4:4:4** (`yuv444p…`): `hevc_2K24P_rext_10bit_444_yuv444p10le_{1,3}`,
  `hevc_4k24P_rext_12bit_444_pq`, `hevc_4K24P_rext_12bit_444_yuv444p12le_1`,
  `quick-brown-fox-…-rext-{8,10,12}bit-444`
- **4:0:0 monochrome** (`gray` / `gray10le` / `gray12le`): `quick-brown-fox-…-rext-{8,10,12}bit-400`
- **12-bit even at 4:2:0** (`yuv420p12le`): `quick-brown-fox-…-rext-12bit-420`

What this needs (rough, unvalidated — the decoder side is the first unknown):
- **Decoder output format.** Confirm what rpivid/`v4l2-request` emits for RExt 4:2:2/4:4:4/12-bit —
  whether it produces a SAND variant at all, and at what bit depth, or rejects them (it accepted
  RExt 4:2:0). If rpivid won't emit these, they fall back to software HEVC decode (see the perf caveat).
- **New unpack kernels.** SAND (or linear) → planar for 4:2:2 / 4:4:4 / mono, and a 12-bit path.
  The existing `av_rpi_sand30_to_planar_*` are 4:2:0-10bit-specific; 4:2:2/4:4:4 have different
  chroma geometry (and 12-bit a different pack). New NEON kernels + `checkasm` coverage per format.
- **Filter + hwcontext plumbing.** `vf_sand_to_yuv420p_drm` currently offers YUV420P out for RPI4_10;
  extend format negotiation + the apply/tone-map paths for the new chroma layouts.
- **Encoder reality check.** The bcm2835 H.264 encoder is **8-bit 4:2:0** only, so 4:2:2/4:4:4/12-bit
  input must be converted down to 8-bit 4:2:0 for encode regardless — the extra chroma/precision can't
  survive to output. So "support" here means *decode + correctly downconvert*, not preserve.
  (Monochrome 4:0:0 → emit 4:2:0 with neutral chroma.)
- **Per-format perf** measured as each lands; real-time not assumed.

Order of attack (suggested): monochrome 4:0:0 (simplest — luma only) → 4:2:2 → 4:4:4 → 12-bit
depth handling (orthogonal, folds into each). Start from whichever the decoder actually emits.

## 2. `FAIL-hwdecode` — >4K resolution (want: optimize eventually; real-time unlikely)

**Goal: not real-time (probably impossible without HW decode), but worth optimizing the software path
down the line.** rpivid rejects the buffer size, so there is no hardware decode — the only route is
software HEVC decode + our NEON unpack + ISP scale + HW encode. Six corpus clips:
- **8K** (7680×4320): `hevc_8k60P_bilibili_{1,2,3}`, `hevc_8k30P_slice`, `hevc_8k10P_slice`
- **5.7K** (5760×3600): `hevc_5.7k60P_slice`

Directions (later):
- Software HEVC decode is the wall (4-core A72, no HW). Measure the ceiling; threaded sw-decode +
  our downscale-fused path (`out=half` won't apply at these ratios; needs an ISP or NEON downscale
  from >4K → ≤1080p) + HW encode. Likely well below real-time but useful for offline/batch.
- Consider decode-then-downscale as early as possible to shrink the pixel volume before the CPU
  touches it (the SAND unpack + tone-map cost scales with input pixels).
- Purely a "handle it at all, then make it as fast as possible offline" track — no real-time target.

---

## Not in scope (hard limits, for the record)
- **Non-HEVC codecs** (H.264 / VP9 / AV1) — rpivid is HEVC-only; out of this pipeline entirely.
- **Output beyond 8-bit 4:2:0 H.264 ≤1080p** — fixed by the bcm2835 encoder (level 4.0, one stream).
- **DV profile 7 dual-layer** — expected to decode its HDR10 base only (EL/RPU ignored); untested, no
  P7 sample in the corpus. Add a P7 sample and verify if it ever matters.
