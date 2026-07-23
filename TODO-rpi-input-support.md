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

## 1. `FAIL-filter` — non-4:2:0 / >10-bit HEVC — **INVESTIGATED (2026-07-23); premise was wrong**

The 16 clips: **4:2:2** (`hevc_4k60P_rext_10bit_422_eos_r5c_1`, `hevc_4k50P`,
`quick-brown-fox-…-rext-{8,10,12}bit-422`), **4:4:4** (`hevc_2K24P_rext_10bit_444_yuv444p10le_{1,3}`,
`hevc_4k24P_rext_12bit_444_pq`, `hevc_4K24P_rext_12bit_444_yuv444p12le_1`,
`quick-brown-fox-…-rext-{8,10,12}bit-444`), **4:0:0 mono** (`quick-brown-fox-…-rext-{8,10,12}bit-400`),
**12-bit 4:2:0** (`quick-brown-fox-…-rext-12bit-420`).

**Finding (empirical, this Pi 4): none of these hardware-decode — so there is NO SAND frame, and no
unpack kernel is needed.** The Argon HEVC block is 4:2:0-only (4:2:2/4:4:4 HW decode is a Pi 5 /
BCM2712 feature). `-hwaccel drm` silently falls back to **software** HEVC decode (`Format yuv422p
chosen by get_format()`), producing plain planar frames, never SAND/DRM_PRIME. The sweep's
"FAIL-filter" was purely an artifact of *forcing* the SAND path (`-hwaccel drm
-hwaccel_output_format drm_prime` + `sand_to_yuv420p_drm`) onto software frames.

**It already works, correctly, with stock ffmpeg — no new code:**
`ffmpeg -i in.mp4 -vf scale=W:H,format=yuv420p -c:v h264_v4l2m2m …` (software HEVC decode → swscale
chroma-downconvert + resize → HW H.264 encode). Verified end-to-end producing valid `h264`/`yuv420p`
output for **4:2:2, 4:4:4, monochrome 4:0:0, and 12-bit (4:2:0 & 4:4:4)** — all exit 0. (Encoder is
8-bit 4:2:0, so the extra chroma/precision is downconverted away regardless — "support" = decode +
correct downconvert, which swscale already does; mono → 4:2:0 with neutral chroma, automatic.)

**So the only real work is ROUTING, not kernels:** the Jellyfin wrapper must detect non-4:2:0 / >10-bit
HEVC and select the software path (plain `-i` + swscale + `h264_v4l2m2m`) instead of the SAND hwaccel
path (which can't apply). No `av_rpi_sand30_*` variant, no `vf_sand_to_yuv420p_drm` change.

**Perf is software-HEVC-decode-bound and resolution-gated:**
- **≤1080p:** software decode is comfortably real-time → usable live. (Functionally confirmed; the
  corpus's only low-res non-4:2:0 clips are 1-frame test patterns, so no throughput figure, but
  720/1080p sw HEVC on the A72 is well within real-time.)
- **4K:** far below real-time — **4K 4:2:2 10-bit 60p measured ~0.05× (3.3 fps)** end-to-end, with
  decode-only ~0.063× (3.7 fps): the swscale downconvert+resize+HW-encode add only ~15%; sw HEVC
  decode is essentially the entire cost. **Nothing our filter/kernel work can move** — this merges
  into the §2 software-decode/offline track (the only lever is faster sw HEVC decode, i.e. upstream).

**Net: no kernels needed.** Deliverables shrink to (a) wrapper routing for non-4:2:0/>10-bit HEVC →
software path; (b) treat 4K+ of these as offline-only (with §2). If an HDR non-4:2:0 clip ever appears
(rare — these are mostly SDR/log camera formats), the software path can chain the CPU `zscale`/tonemap.

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
