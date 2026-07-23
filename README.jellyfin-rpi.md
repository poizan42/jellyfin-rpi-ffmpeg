# Jellyfin / Raspberry Pi 4 fork — additions

This fork adds a **real-time HEVC→H.264 downscaling transcode path** for the Raspberry Pi 4
(BCM2711, Cortex-A72, VideoCore VI) on top of Jellyfin FFmpeg 8.1.2. Everything below is
additive to upstream; the base FFmpeg is unchanged except where noted.

## The pipeline

```
HEVC (10/8-bit)                         8-bit H.264
   │                                        ▲
   ▼                                        │
rpivid HW decode ──DRM_PRIME──►  NEON unpack  ──DRM_PRIME──►  ISP downscale ──DRM_PRIME──►  bcm2835 H.264 encode
(/dev/video19, SAND30/SAND)   (vf_sand_to_yuv420p_drm)     (scale_v4l2m2m,               (/dev/video11, 8-bit)
                               SAND → 8-bit YU12            /dev/video12, YU12 only)
```

The decoder emits **SAND** — Broadcom's column-tiled format (128-byte stripes; for 10-bit,
`SAND30`/`AV_PIX_FMT_RPI4_10`: three 10-bit samples per 32-bit word). Neither the ISP scaler
(8-bit-YUV + DRM_PRIME only) nor the H.264 encoder can ingest it, so the **one** unavoidable
CPU step is unpacking SAND → planar 8-bit YU12. This fork makes that step fast (NEON, threaded)
and keeps everything else on fixed-function hardware, zero-copy.

## Hardware map (why each op lands where it does)

| engine | device | role |
|---|---|---|
| rpivid (Argon) | `/dev/video19`, `/dev/media*` | HEVC Main/Main10 decode → SAND DRM_PRIME |
| Cortex-A72 NEON | — | SAND → planar 8-bit unpack (bandwidth/latency-bound; the CPU cost) |
| bcm2835 ISP | `scale_v4l2m2m`, `/dev/video12` | fixed-function downscale (8-bit YUV, DRM_PRIME in/out only) |
| bcm2835 codec | `h264_v4l2m2m`, `/dev/video11` | 8-bit H.264 encode |
| V3D (Vulkan) | `/dev/dri/renderD128` | evaluated for offload — **lost** (scattered-read latency); not used |

## What this fork adds

### 1. NEON SAND30 → planar kernels  (`libavutil/`)
Direct 10-bit `SAND30` → 8-bit planar conversion, bit-exact (`checkasm --test=rpi_sand`):
- `av_rpi_sand30_to_planar_y8` / `_c8` — `libavutil/aarch64/rpi_sand_neon.S` (+ 32-bit `arm/`),
  wrappers/dispatch in `libavutil/rpi_sand_fns.c`, `RPI4_10 → YUV420P` path.
- `hwcontext_drm.c` offers `YUV420P` for `RPI4_10` downloads.
- **Software prefetch** of the ~400 KB-stride SAND stripe reads in the sand30 loops
  (`prfm pldl1strm`) — hides DRAM latency; +8–9% single-thread (inert; checkasm unchanged).

### 2. The NEON→ISP bridge filter  `vf_sand_to_yuv420p_drm`  (`libavfilter/`)
The core of the pipeline. Takes the decoder's SAND `DRM_PRIME` frame and emits an 8-bit linear
**YU12 `DRM_PRIME`** frame the ISP scaler accepts — so the resize runs on the ISP instead of
eating ~1.2 CPU cores as swscale. Properly `configure`-integrated
(`CONFIG_SAND_TO_YUV420P_DRM_FILTER`, `sand_to_yuv420p_drm_filter_select="sand"`). Details:
- **Slice-threaded** unpack (per-row-independent bands; `AVFILTER_FLAG_SLICE_THREADS`).
- **Input mmap-cache** — the decoder recycles a fixed set of SAND dma-buf fds, so map them once
  instead of `mmap`+invalidate+`munmap` of ~16 MB per frame via `av_hwframe_map`.
- **Thread cap** (`nb_threads-1`) — leaves a core for the decode/encode threads (the memory-bound
  unpack over-subscribes the 4-core chip otherwise).
- Pooled CMA dma-bufs (dma-heap) for the output; DRM_PRIME → zero-copy into the encoder.
- `SAND_PROF=1` env var: per-phase profiler (map / unpack / flush / unmap μs/frame).

### 3. HDR→SDR tone-mapping  (`tm=none|fast|accurate` option on the bridge filter)
Without it, HDR10 (PQ/BT.2020) sources transcode with a plain 10→8-bit truncation → washed-out.
Two tiers, both tuned to match FFmpeg's `zscale+tonemap=hable` (chosen by eye) and **baked
directly from that chain** into embedded LUTs (`libavfilter/rpi_tonemap_gen.py` →
`rpi_tonemap_tables.h`; re-runnable):
- `tm=fast` — **real-time** (1.11× at 4K HDR, within 4% of `tm=none`). Luma-exact 1D tone curve
  **folded into the single-pass SAND30 NEON unpack** (`ff_rpi_sand30_lines_to_planar_y8_lut`:
  64-entry `tbl` + lerp in place of the `>>2` narrow, no 10-bit intermediate) + separable
  chroma; colour approximate. Bit-exact vs a scalar oracle (`checkasm --test=rpi_sand`).
- `tm=accurate` — chroma-resolution 3D LUT (luma-aware), reproduces zscale; ~¼ the cost of a
  full 4:4:4 LUT for the same quality. Applied with **fixed-point tetrahedral** interpolation
  (4 taps, all-integer — more accurate near saturated corners than trilinear, and faster).
  Quality tier, **not** real-time by design (~0.77× at 4K HDR). The apply is NEON (8 chroma
  samples/iter: branchless tetrahedron select + software gather); it's ultimately gather-latency
  bound, so it stays below the fast tier.
- `tm=none` (default) — plain truncation, byte-for-byte unchanged. **Exception:** a Dolby Vision
  profile 5 source always engages the DV path below (truncating it would corrupt colour).

Both tiers select their tone LUTs by the frame's transfer: **PQ/HDR10** (SMPTE2084) uses the
`ff_rpi_tm_*` set, **HLG** (ARIB_STD_B67, incl. DV P8.4's HLG base) uses a separately baked
`ff_rpi_tm_*_hlg` set — same hable chain and grid, only the source EOTF differs, so HLG is
linearized correctly instead of being mis-read through the PQ curve. Both sets are baked in one
`rpi_tonemap_gen.py` run. HLG validated vs the zscale HLG→SDR oracle at 41/49/54 dB (Y/Cb/Cr),
matching the PQ tier's own agreement with its oracle; PQ output stays byte-identical.

**Dolby Vision profile 5** is handled automatically (no option): the P5 base layer is Dolby's
*reshaped* IPT-PQ signal, not HDR10, so it's reconstructed to HDR10 from the per-frame RPU
metadata (`AV_FRAME_DATA_DOVI_METADATA`) and then tone-mapped. Detected by RPU-present +
base-not-HDR10-tagged. A 33³ base-YCbCr→SDR LUT is baked once per scene (all the DV colour math —
poly/MMR reshape, `ycc_to_rgb`, PQ, HPE·`rgb_to_lms` — in scalar C, off the hot path, composed
with the same zscale/hable tone LUT), then applied with NEON fixed-point tetrahedral (luma is a
full 3D lookup since P5 luma is cross-channel). Validated bit-close to libplacebo
(`apply_dolbyvision`): HDR10 decode ≥62 dB PSNR, end-to-end SDR ≥51 dB. Perf ~0.60× at 4K
(luma 3D lookup is heavier than HDR10's 1D luma; gather-bound). P8 (HDR10-tagged) uses the
HDR10 tonemap above. See `TODO-rpi-tonemap.md`.

P5 honours the `tm=` knob: `tm=none`/`accurate` (default) → the full 3D luma+chroma path above;
**`tm=fast`** → a faster tier that approximates luma with a 1D neutral-chroma curve (baked per-RPU
alongside the 3D LUT) while keeping chroma the full 3D path, so it drops the ~8.3M/frame 3D luma
lookups. **~1.0× at 4K (real-time)**; luma within ~45–51 dB of the 3D path on real frames
(chroma bit-identical). Colour-approximate but correct-hued — the accurate path stays the default.

Deferred (see `TODO-rpi-tonemap.md`): command-line-tunable peak/operator/saturation, a BT.2390
operator (`op=bt2390`), non-1000-nit peaks, 32-bit ARM parity for the tone LUTs.

### 4. Build enablement for the HDR reference
`--enable-libzimg` (`zscale`) gives a correct CPU HDR→SDR reference (`zscale+tonemap`).
`--enable-libplacebo --enable-vulkan` link a **locally rebuilt** libplacebo 7.360 (Debian
bookworm ships 4.208, too old for FFmpeg 8.x; its shaderc is also broken).
Note: the libplacebo *filter* does not run on the Pi's V3DV (missing renderable formats), so the
usable HDR reference here is the CPU `zscale+tonemap` chain.

## Building

Baseline configure used for this fork (Pi 4, aarch64, Debian bookworm):

```sh
./configure --disable-doc --enable-libdrm --enable-libudev --enable-sand --enable-v4l2-request \
            --enable-libzimg --enable-libplacebo --enable-vulkan \
            --extra-cflags="-I<path>/vulkan-1.4-headers/include"
make -j4
```

- `--enable-sand` + `--enable-v4l2-request` are required for the SAND filter and rpivid decode.
- `--enable-libzimg`/`--enable-libplacebo`/`--enable-vulkan` are only needed for the HDR
  *reference* (not for `tm=fast`/`accurate`, which are self-contained NEON + embedded LUTs).
- libplacebo/glslang were rebuilt from Debian *forky* sources against bookworm; the extra Vulkan
  1.4 headers are needed by libplacebo 7.360.

Bit-exact + microbench of the SAND kernels: `tests/checkasm/checkasm --test=rpi_sand [--bench]`.

## Usage

Real-time HEVC→H.264 720p transcode (the shipped SDR path):

```sh
ffmpeg -hwaccel drm -hwaccel_output_format drm_prime -i in.mkv \
       -vf sand_to_yuv420p_drm,scale_v4l2m2m=1280:720 \
       -c:v h264_v4l2m2m -b:v 3M out.mp4
```

HDR10 source — add `tm=fast` (real-time) or `tm=accurate` (quality):

```sh
       -vf sand_to_yuv420p_drm=tm=fast,scale_v4l2m2m=1280:720
```

## Performance (Pi 4B, 600-frame steady-state, → 720p)

| source | speed | notes |
|---|---:|---|
| 10-bit HEVC SDR 1080p | ~2.0–2.7× | decode-thread-bound, not CPU-bound |
| 10-bit HEVC SDR 4K scope (3840×1608) | ~1.42× | slice-thread + prefetch + map-cache + thread-cap |
| 10-bit HEVC HDR10 4K (3840×2160), `tm=none` | ~1.16× | truncation, colour wrong |
| 10-bit HEVC HDR10 4K (3840×2160), `tm=fast` | ~1.17× | **real-time, correct colour** (single-pass tone-map fold) |
| 10-bit HEVC HDR10 4K (3840×2160), `tm=accurate` | ~0.94× | quality tier (NEON tetrahedral 3D-LUT); near real-time |
| 10-bit HEVC Dolby Vision **profile 5** 4K (3840×2160), default/`tm=accurate` | ~0.60× | correct colour via per-RPU 3D LUT + NEON tetrahedral (luma is a full 3D lookup); scalar was 0.15× |
| 10-bit HEVC Dolby Vision **profile 5** 4K (3840×2160), `tm=fast` | ~1.0× | **real-time**; 1D neutral-chroma luma approx + full 3D chroma; luma ~45–51 dB vs accurate, chroma identical |

The 4K unpack is **memory-latency-bound** on the scattered SAND reads (same wall that made the
V3D GPU offload lose). Threading reaches the shared-bus ceiling with ~2–3 cores; the levers above
free the rest of the machine for decode/encode. The tone-map tiers process the 10-bit scratch in
small **`TM_CHUNK`=4-row L1-resident tiles** — a swept knee (16 rows thrashes L2, ~+14–18% slower);
this is what brings `tm=accurate`/DV-P5-`fast` up to (near) real-time, bit-identically.
