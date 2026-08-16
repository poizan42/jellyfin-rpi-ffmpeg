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

## Supported input

What the pipeline can ingest is fixed by the hardware above: the **rpivid** decoder (HEVC
Main/Main10, ≤4K) and the **4:2:0-only** SAND→YU12 unpack. The encoder is 8-bit H.264 4:2:0 ≤1080p,
so any 4K source is downscaled first (`scale_v4l2m2m`, or `out=half` for exact 2:1).

| dimension | supported | not supported |
|---|---|---|
| codec | **HEVC** (rpivid) | H.264 / VP9 / AV1 (rpivid is HEVC-only) |
| chroma + depth | **4:2:0, 8-bit or 10-bit** | 4:2:2, 4:4:4, 4:0:0 (mono), 12-bit |
| profile | Main, Main 10, **and Range-Extensions *if* the format is 4:2:0 8/10-bit**; Main Still Picture; MV-HEVC (base view) | any profile whose *pixel format* is outside 4:2:0 8/10-bit |
| resolution | up to 4K (3840×2160) | 5.7K, 8K (decoder rejects the buffer size) |
| dynamic range | SDR (BT.709), HDR10 (PQ), HLG, **Dolby Vision P5** (RPU→HDR10) and P8.1 (HDR10 base) | — (DV P7 dual-layer expected to decode its HDR10 base only, EL/RPU ignored — untested, no P7 sample) |
| output | 8-bit H.264 4:2:0, **≤1080p** (downscale 4K first) | 4K H.264 (encoder is 1080p/level-4.0) |

**Gate on pixel format, not profile string.** A `Range Extensions`-tagged stream that is actually
4:2:0 8/10-bit decodes and transcodes fine — only the chroma/bit-depth outside 4:2:0 8/10-bit is the
real limit. (Empirically confirmed against the ByteDance HEVC demo corpus — 59 clips in
`samples/ByteDance-HEVC/` with per-clip probe data in `videos.json`: 37 transcode on the **hardware**
path, the 22 that don't are exactly the 8K/5.7K set and the RExt 4:2:2/4:4:4/mono/12-bit set.)

"Not supported" here means **not via this HW path** — those inputs still transcode via stock
*software* HEVC decode (`ffmpeg -i in -vf scale,format=yuv420p -c:v h264_v4l2m2m`; only the encode is
HW), so it's a wrapper-routing question, **not new kernels** (the Pi 4 never hardware-decodes them —
no SAND frame is produced). But software HEVC decode is the wall and it's **below real-time even at
1080p** (1080p 4:4:4 10-bit ≈0.48× / 12 fps; 4K 4:2:2/4:4:4 ≈0.05–0.10×), so treat this whole group
as **offline/batch on the Pi 4**, not live. See [`rpi-notes/pipeline/input-support.md`](rpi-notes/pipeline/input-support.md).

## Path selection

Which route an input takes follows directly from its **pixel format + resolution** (per the matrix
above) plus the target size. `tools/rpi-transcode-path.sh <input> [WxH]` is the reference
implementation of this rule (validated below); the production selector lives in the Jellyfin
transcoding wrapper.

| condition (HEVC input) | route | ffmpeg shape |
|---|---|---|
| 4:2:0 8/10-bit, ≤4K | **HW** (rpivid+SAND, real-time) | `-hwaccel drm -hwaccel_output_format drm_prime -i IN -vf sand_to_yuv420p_drm=tm=<tm>[…] -c:v h264_v4l2m2m` |
| non-4:2:0 / 12-bit, or >4K | **SW** (software decode, offline) | `-i IN -vf [TM,]scale=W:H,format=yuv420p -c:v h264_v4l2m2m` |
| non-HEVC | out of scope (rpivid is HEVC-only) | — |

On the **HW path** the flags derive as:
- `tm` = `none` for SDR, `fast` (real-time) for HDR, where **HDR = `color_transfer` ∈ {smpte2084,
  arib-std-b67} OR Dolby Vision present**. DV must be read from *side data* — a DV P5 stream reports
  `color_transfer=unknown`, so a transfer-only test misses it (the filter reconstructs P5/P8 regardless,
  but detecting DV is what selects the fast real-time tier over the slow default). `accurate` is opt-in.
- downscale: **exact 2:1** (`target == input/2`, dims multiples of 4) → `:out=half` (fused, drops
  `scale_v4l2m2m`); any other downscale → `,scale_v4l2m2m=W:H`; none → no scale. Target must be ≤1080p
  (encoder cap).

On the **SW path** (offline — sub-real-time: 1080p 4:4:4 10-bit ≈0.48×, 4K ≈0.05–0.10× on the pre-MC-NEON
C decoder) HDR additionally needs a CPU tone-map (`zscale=t=linear:npl=100,tonemap=hable,zscale=t=bt709…`;
needs a `zscale`-enabled build). The 10-bit software decode is now **~1.3× faster** (thermal-fair A/B,
4K 4:2:2) since the H.26x motion-comp kernels (uni/put/bi, all widths) got NEON — still offline, but less
so; CABAC is now the wall. See [`rpi-notes/pipeline/input-support.md` §4](rpi-notes/pipeline/input-support.md#4-software-hevc-decode--profiled-neon-gaps-the-productive-lever-for-12).

**Validated** against the 59-clip ByteDance corpus (`samples/ByteDance-HEVC/`): the selector routes
**37→HW / 22→SW** at a 720p target — matching every clip's measured `hw_transcode` verdict (0 mismatches)
— and emits `out=half` for all 30 4K 4:2:0 clips at a 1080p target.

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
- **Input mmap-cache** — the decoder recycles a fixed set of SAND dma-bufs, so map them once
  instead of `mmap`+invalidate+`munmap` of ~16 MB per frame via `av_hwframe_map`. Keyed on the
  dma-buf **inode** (fd numbers get recycled and would alias a freed buffer), LRU-evicted, and it
  handles a frame described by **any number of objects** — the RPi decoder splits luma and chroma
  into separate objects on some kernels, and a cache that insists on one object silently falls
  back to per-frame mapping, which costs ~15 ms/frame at 4K. `SAND_PROF=1` prints `[mmaps=N]`,
  which must plateau; `mmaps=0` means the cache is being bypassed.
- **Thread cap** (`nb_threads-1`) — leaves a core for the decode/encode threads (the memory-bound
  unpack over-subscribes the 4-core chip otherwise).
- Pooled CMA dma-bufs (dma-heap) for the output; DRM_PRIME → zero-copy into the encoder.
- `SAND_PROF=1` env var: per-phase profiler (map / unpack / flush / unmap μs/frame).

### 3. HDR→SDR tone-mapping  (`tm=none|fast|veryfast|accurate` option on the bridge filter)
Without it, HDR10 (PQ/BT.2020) sources transcode with a plain 10→8-bit truncation → washed-out.
Two HDR10 tiers (`veryfast` adds a third, DV-P5-only tier — see below), both tuned to match
FFmpeg's `zscale+tonemap=hable` (chosen by eye) and **baked
directly from that chain** into embedded LUTs (`libavfilter/rpi_tonemap_gen.py` →
`rpi_tonemap_tables.h`; re-runnable):
- `tm=fast` — **real-time** (~1.17× at 4K HDR, essentially matching `tm=none`'s ~1.16×). Luma-exact 1D tone curve
  **folded into the single-pass SAND30 NEON unpack** (`ff_rpi_sand30_lines_to_planar_y8_lut`:
  64-entry `tbl` + lerp in place of the `>>2` narrow, no 10-bit intermediate) + separable
  chroma; colour approximate. Bit-exact vs a scalar oracle (`checkasm --test=rpi_sand`).
- `tm=accurate` — chroma-resolution 3D LUT (luma-aware), reproduces zscale; ~¼ the cost of a
  full 4:4:4 LUT for the same quality. Applied with **fixed-point tetrahedral** interpolation
  (4 taps, all-integer — more accurate near saturated corners than trilinear, and faster).
  Quality tier, near real-time (~0.94× at 4K HDR after `TM_CHUNK`=4 L1-tiling). The apply is NEON (8 chroma
  samples/iter: branchless tetrahedron select + software gather); it's ultimately gather-latency
  bound, so it stays below the fast tier.
- `tm=none` (default) — plain truncation, byte-for-byte unchanged. It means what it says for
  **every** source, Dolby Vision included: no tone-map, no P5 reconstruction, no LUT bake. That
  makes it the unpack-only baseline for measurement — and wrong output for any HDR source, since
  a 10→8 truncation cannot fit a wide dynamic range into SDR. On DV P5 it is *badly* wrong
  rather than merely flat (the base layer is Dolby's reshaped IPT-PQ), so the filter logs a
  one-shot warning.

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
HDR10 tonemap above. See [`rpi-notes/pipeline/tonemap.md`](rpi-notes/pipeline/tonemap.md).

P5 honours the `tm=` knob: `accurate` (default) → the full 3D luma+chroma path above;
**`tm=fast`** → a faster tier that approximates luma with a 1D neutral-chroma curve (baked per-RPU
alongside the 3D LUT) while keeping chroma the full 3D path, so it drops the ~8.3M/frame 3D luma
lookups. **~1.0× at 4K (real-time)**; luma within ~45–51 dB of the 3D path on real frames
(chroma bit-identical). Colour-approximate but correct-hued — the accurate path stays the default.
**`tm=veryfast`** → like fast, but the 3D chroma tetrahedral apply is replaced by a **nearest grid
cell** lookup with **ordered (Bayer 8×8) coordinate dither** (one gather, no 4-tap blend): **~1.06×
at 4K** (+~6% over fast — real headroom past real-time so background load doesn't stutter playback).
The dither dissolves the grid-snapping into sub-visible grain rather than the hard chroma blotches a
plain nearest lookup shows on smooth gradients (skies, water): perceptually (spatially-averaged) it's
within ~63 dB of the tetrahedral chroma, trading banding for low-amplitude grain (a small chroma-bitrate
cost). The dither is a pure function of pixel position → deterministic (md5-reproducible) and
NEON==scalar bit-exact, and free (it's just the round's `+128` becoming a per-pixel bias). Opt-in speed
tier; fast/accurate stay the defaults. On HDR10/HLG sources `tm=veryfast` is identical to `tm=fast`
(their chroma is already the cheap separable path — only P5's cross-channel 3D chroma has anything to drop).

Deferred (see [`rpi-notes/pipeline/tonemap.md`](rpi-notes/pipeline/tonemap.md)): command-line-tunable peak/operator/saturation, a BT.2390
operator (`op=bt2390`), non-1000-nit peaks, 32-bit ARM parity for the tone LUTs.

### 4. Build enablement for the HDR reference
`--enable-libzimg` (`zscale`) gives a correct CPU HDR→SDR reference (`zscale+tonemap`).
`--enable-libplacebo --enable-vulkan` link a **locally rebuilt** libplacebo 7.360 (Debian
bookworm ships 4.208, too old for FFmpeg 8.x; its shaderc is also broken).
Note: the libplacebo *filter* does not run on the Pi's V3DV (missing renderable formats), so the
usable HDR reference here is the CPU `zscale+tonemap` chain.

## Building

### Production configure (Pi 4, aarch64, Debian bookworm)

This is what the deployed binary is built with — it must be **feature-complete enough to run
any command line Jellyfin would have handed to the stock jellyfin-ffmpeg**, because the
[transcode-orchestrator](../transcode-orchestrator/README.md) shim rewrites only the *video*
side of the graph and passes everything else (notably `-codec:a`) through untouched:

```sh
./configure --disable-doc --enable-gpl --enable-version3 \
  --enable-sand --enable-v4l2-request --enable-libdrm --enable-libudev \
  --enable-libzimg --enable-libplacebo --enable-vulkan \
  --enable-libx264 --enable-libx265 --enable-libdav1d --enable-libvpx --enable-libwebp \
  --enable-libmp3lame --enable-libopus --enable-libvorbis --enable-libfdk-aac \
  --enable-libass --enable-libfreetype --enable-libfontconfig --enable-libfribidi --enable-libharfbuzz \
  --enable-gnutls --enable-libxml2 --enable-libbluray \
  --enable-libzvbi --enable-chromaprint --enable-libopenmpt --enable-gmp \
  --extra-cflags=-I<path>/vulkan-1.4-headers/include
make -j4                                    # ~25 min from clean
```

The last line of enables closes gaps against the stock build: `libzvbi`
(teletext subtitles), `chromaprint` (fingerprint muxer), `libopenmpt` (tracker
modules), `gmp` (encrypted RTMP: `rtmpe`/`rtmpte`/`ffrtmpcrypt`).

**`--enable-libfdk-aac` is not optional in practice.** Jellyfin *prefers*
`libfdk_aac` for AAC output when its startup probe finds it, so a fork without it
fails a real transcode with `Unknown encoder 'libfdk_aac'`. FFmpeg upstream
classes fdk-aac as nonfree (needing `--enable-nonfree`, which would make the
binary non-redistributable), but **jellyfin-ffmpeg's patch
`0026-remove-fdk-aac-from-nonfree` moves it to the free library list** — which is
how their own .deb ships it. That patch is applied here (by hand: its second hunk
misses on context drift, though the change is just moving one line between two
lists), so `CONFIG_LIBFDK_AAC=yes` with no `CONFIG_NONFREE` and the build stays
GPLv3. Needs `libfdk-aac-dev` (Debian main). Two things the
stock build has cannot be matched — `pp` (libpostproc) and the `hls` *protocol*
were both removed in FFmpeg 8.0, and this fork is 8.x while the packaged
jellyfin-ffmpeg is 7.x. The hls muxer/demuxer, which is what Jellyfin uses, are
present.

### The jellyfin-ffmpeg patch series is applied in-tree

jellyfin-ffmpeg ships its features as a **quilt series in `debian/patches/`,
applied at Debian build time** — not in the git tree. Building this fork straight
from the tree therefore produced a binary with *none* of them: no `tonemapx` (the
filter Jellyfin puts in its software HDR chains), no `alphasrc`, no AC-4 decoder,
no `0027-pass-dovi-sidedata-to-hlsenc-and-mpegtsenc` — which sits directly on top
of the Dolby Vision and SPS/PPS work here.

The series is **applied as one commit on `jellyfin-rpi`** (94 of 96; the Rockchip
RK3588 patch is skipped as the wrong platform, and `0026-remove-fdk-aac-from-nonfree`
is applied by hand — see the libfdk-aac note above). It is jellyfin's own series and
current for this base, so it is written against exactly this tree and applies without
rejects.

**Rebasing onto a newer jellyfin-ffmpeg:** revert that commit, merge the new base,
re-apply the series, commit again:

```sh
git revert --no-commit <series-commit>
git merge jellyfin-upstream/jellyfin          # or the new tag
while read -r p; do patch -p1 --forward -i "debian/patches/$p"; done < debian/patches/series
git add -u && git commit
```

Deliberate departures from stock jellyfin-ffmpeg: **static, not `--enable-shared`** (so
`/opt/rpi-ffmpeg-orchestrator/ffmpeg-real` is one self-contained binary that `install.sh`
can just copy), no LTO, and none of the other vendors' hardware —
cuda/nvenc/cuvid/rkmpp/qsv/vaapi/amf/opencl, unusable on a Pi. The only codecs left out are
`libsvtav1` (no AV1 encode target here), `libtheora`, and the experimental `sonic`/`sonicls`
encoders. Everything else the stock build has is built, which
`transcode-orchestrator/check-capabilities.sh` verifies.

**Build full-featured for anything you deploy.** The orchestrator shim rewrites only the
video side of a Jellyfin command line and passes the rest — notably `-codec:a` — through to
this binary, so a missing encoder is a hard `Encoder not found` at playback rather than a
slow transcode. `transcode-orchestrator/check-capabilities.sh` diffs this build against the
stock jellyfin-ffmpeg and `install.sh` refuses to deploy on an unexplained gap. The lean
configure below is for fast iteration only.

### Minimal configure (fast iteration on the video kernels only)

```sh
./configure --disable-doc --enable-libdrm --enable-libudev --enable-sand --enable-v4l2-request \
            --enable-libzimg --enable-libplacebo --enable-vulkan \
            --extra-cflags="-I<path>/vulkan-1.4-headers/include"
make -j4
```

**Do not deploy this build** — see the capability requirement above.

- `--enable-sand` + `--enable-v4l2-request` are required for the SAND filter and rpivid decode.
- `--enable-libzimg`/`--enable-libplacebo`/`--enable-vulkan` are only needed for the HDR
  *reference* (not for `tm=fast`/`accurate`, which are self-contained NEON + embedded LUTs).
  (They will also drive peak-aware LUT generation and the SW-decode HDR tone-map — see
  [`rpi-notes/pipeline/tonemap.md`](rpi-notes/pipeline/tonemap.md).)
- libplacebo/glslang were rebuilt from Debian *forky* sources against bookworm; the extra Vulkan
  1.4 headers are needed by libplacebo 7.360. **`--enable-vulkan` fails on bookworm's stock Vulkan
  headers (1.3.239 → "vulkan requested but not found")** — hence the 1.4 `--extra-cflags` include.
- **Runtime (important):** libplacebo/Vulkan filtering must use the **lavapipe (llvmpipe) software
  Vulkan** ICD — `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json` with
  `-init_hw_device vulkan`. The Pi's **V3D** Vulkan driver can't do the FFmpeg/libplacebo
  external-memory interop (`hwupload` → `VK_ERROR_INVALID_EXTERNAL_HANDLE`). This is why libplacebo
  is only a CPU-side *reference/LUT-gen* tool here, not a per-frame filter.

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

**Exact 2:1 (4K→1080p): use `out=half` instead of the ISP scaler** — the filter fuses the 2×2
downscale into the tone-map and emits 1080p directly (no `scale_v4l2m2m`), which makes even DV P5
`tm=accurate` real-time (see below):

```sh
       -vf sand_to_yuv420p_drm=tm=accurate:out=half
```

## Performance (Pi 4B, 600-frame steady-state, → 720p)

Measured on kernel 6.18.44.

| source | speed | notes |
|---|---:|---|
| 10-bit HEVC SDR 1080p | ~2.0–2.7× | decode-thread-bound, not CPU-bound |
| 10-bit HEVC SDR 4K scope (3840×1608) | **1.64×** | slice-thread + prefetch + map-cache + thread-cap. With `out=half` first: **2.11×** |
| 10-bit HEVC HDR10 4K (3840×2160), `tm=none` | **1.23×** | truncation — wrong colour, and no tone-map work: this is the unpack-only baseline |
| 10-bit HEVC HDR10 4K (3840×2160), `tm=fast` | **1.27×** | **real-time, correct colour** (single-pass tone-map fold) |
| 10-bit HEVC HDR10 4K (3840×2160), `tm=accurate` | **1.02×** | quality tier (NEON tetrahedral 3D-LUT), real-time |
| 10-bit HEVC Dolby Vision **profile 5** 4K (3840×2160), default/`tm=accurate` | **0.69×** | correct colour via per-RPU 3D LUT + NEON tetrahedral (luma is a full 3D lookup) |
| 10-bit HEVC Dolby Vision **profile 5** 4K (3840×2160), `tm=fast` | **1.03×** | **real-time**; 1D neutral-chroma luma approx + full 3D chroma; luma ~45–51 dB vs accurate, chroma identical |
| 10-bit HEVC Dolby Vision **profile 5** 4K (3840×2160), `tm=veryfast` | **1.11×** | **real-time + headroom**; fast luma + ordered-dithered nearest-cell 3D chroma; dither de-bands the grid-snapping into grain (perceptually ~63 dB vs tetrahedral, deterministic) |

At a **720p target the shim uses `out=half`** (4K ≥ 2× the target), which is faster
still than every row above: DV P5 `tm=veryfast` **1.46×**, HDR10 `tm=veryfast`
**1.53×**. The rows here isolate the tone-map tiers at full 4K apply + ISP scale.

The 4K unpack is **memory-latency-bound** on the scattered SAND reads (same wall that made the
V3D GPU offload lose). Threading reaches the shared-bus ceiling with ~2–3 cores; the levers above
free the rest of the machine for decode/encode. The tone-map tiers process the 10-bit scratch in
small **`TM_CHUNK`=4-row L1-resident tiles** — a swept knee (16 rows thrashes L2, ~+14–18% slower);
this is what brings `tm=accurate`/DV-P5-`fast` up to (near) real-time, bit-identically.

### 4K → 1080p — and the `out=half` fused downscale

For an exact 2:1 downscale (4K→1080p), the filter can **emit 1080p directly** (`out=half`): it
box-averages 2×2 right after the SAND unpack, runs the tone-map apply at 1920×1080 / 960×540, and
hands a 1080p dma-buf straight to the encoder — **dropping the `scale_v4l2m2m` ISP stage entirely**.
This both shrinks the per-sample apply ~4× *and* removes the ISP's ~15.5 MB/frame of bus traffic,
which was *contending* with the memory-latency-bound unpack (the identical filter runs ~27 ms/frame
alone but ~40 ms in-pipeline). Net: it turns the whole 4K→1080p DV/HDR path real-time.

Measured on kernel 6.18.44, 600 frames (SDR: She-Hulk bt709 scope, half = 1920×804;
HDR10: Agatha S01E01; DV P5: Agatha S01E05).

| source (4K → 1080p) | 4K apply + ISP scale | **`out=half`** (fused, no ISP) |
|---|---:|---:|
| SDR, `tm=none` | **1.38×** | **1.84×** |
| HDR10, `tm=fast` | **1.07×** | **1.37×** |
| HDR10, `tm=accurate` | **0.91×** | **1.28×** |
| DV **profile 5**, default/`tm=accurate` | **0.64×** | **1.16×** |
| DV **profile 5**, `tm=veryfast` | **0.96×** (0.90–0.93) | **1.32×** (1.19–1.25) |

`out=half` uses **tetrahedral chroma at half-res for every P5 tier** (4× fewer chroma sites make it
cheaper than the full-res nearest path *and* higher quality — so `fast`/`veryfast` collapse to the
tetra path and the nn/dither tier isn't used on this path; it remains for 720p / non-2:1 outputs).
Quality vs the accurate reference downscaled: ordering error is negligible (Y/Cb/Cr ~57–60 dB, max ~2
codes — downscale-then-tone-map is the same order libplacebo/mpv use); box 2×2 gives a slight softening
vs the ISP's polyphase. Output is deterministic and NEON==scalar bit-exact; `out=full` (default) is
byte-identical to before. **So at 4K→1080p, even DV P5 `tm=accurate` (the quality default) is now
real-time**.

The dominant cost of the CPU filter is the **fixed 4K SAND unpack** (memory-latency-bound; unchanged
by `out=half`, which is why the 4K→720p 3:1 case — non-integer, still full-res + ISP — keeps the
`veryfast`+dither tier). `out=half` only helps the exact-2:1 path, but there it's decisive.

### Software-decode path (offline)

(Optimization log, shipped wins and negative results for this path: [`rpi-notes/sw-decode/optimization.md`](rpi-notes/sw-decode/optimization.md);
entropy-decode deep dive: [`rpi-notes/sw-decode/cabac-simd.md`](rpi-notes/sw-decode/cabac-simd.md).)

The formats rpivid can't decode — non-4:2:0 (4:2:2 / 4:4:4), 12-bit, or >4K — fall back to **software
HEVC decode** (+ swscale + `h264_v4l2m2m`). These are all **offline / sub-real-time** and always will be
(CABAC is the serial wall), but they benefit from the 10/12-bit MC NEON added in this fork (see
[`rpi-notes/pipeline/input-support.md` §4](rpi-notes/pipeline/input-support.md#4-software-hevc-decode--profiled-neon-gaps-the-productive-lever-for-12)) and the **12-bit inverse-transform NEON** added here (the 12-bit
`idct[]` was falling back to C — only `idct_dc` was wired). Measured on this Pi 4B, current build, real samples
(`samples/ByteDance-HEVC/`), decode → `scale=1280:720` → `format=yuv420p` → `h264_v4l2m2m`, steady-state
(150–250 frames), **no HDR tone-map** (raw pipeline throughput):

| input (→ 720p) | why SW | sample | fps | speed |
|---|---|---|---:|---:|
| 4K 4:2:2 10-bit (60p) | 4:2:2 | `hevc_4k60P_rext_10bit_422_eos_r5c_1` | 6.4 | **0.11×** |
| 4K 4:2:2 10-bit (50p) | 4:2:2 | `hevc_4k50P` | 5.2 | 0.10× |
| 4K 4:4:4 12-bit (24p) | 4:4:4 + 12-bit | `hevc_4K24P_rext_12bit_444_yuv444p12le_1` | 5.4 | **0.22×** |
| 1080p 4:4:4 10-bit (24p) | 4:4:4 | `hevc_2K24P_rext_10bit_444_yuv444p10le_1` | 14 | **0.59×** |
| 5.7K 4:2:0 8-bit (60p) | >4K | `hevc_5.7k60P_slice` | 8.4 | 0.14× |
| 8K 4:2:0 8-bit (30p) | >4K | `hevc_8k30P_slice` | 4.9 | 0.16× |

(`speed` is normalised to the source frame-rate, so 0.11× = 11 % of real-time.) These are up from the
pre-MC-NEON C decoder (1080p 4:4:4 ≈0.48× → 0.59×; 4K 4:2:2 the top of the ≈0.05–0.10× range) — the
~1.3× decode win, diluted end-to-end by the (unchanged) scale + encode stages. The **4K 4:4:4 12-bit**
row is 0.18× → 0.22× from the new 12-bit IDCT NEON: `idct_32x32_12` was ~21 % of that decode's CPU and
is now ~7.5× faster (`checkasm --test=hevc_idct --bench`), bit-exact vs the C reference. (SAO 10/12-bit
NEON was scoped but **dropped after profiling** — the SAO filter measured <0.1 % on these clips.) HDR sources additionally
need a CPU tone-map (`zscale`+`tonemap`, absent from the lean build), which lowers these further. The
practical takeaway is unchanged: **these formats are batch/offline on the Pi 4, not live** — real-time
is only the HW 4:2:0-8/10-bit ≤4K path above.
