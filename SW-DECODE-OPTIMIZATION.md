# Software-decode path — optimization log & experiments (RPi 4, aarch64)

Scope: the **software HEVC decode** transcode path — the inputs the hardware pipeline
can't handle (see [`README.jellyfin-rpi.md`](README.jellyfin-rpi.md)): **4:2:2 / 4:4:4**, **12-bit**, or **>4K**.
These fall back to CPU HEVC decode → swscale (downscale + convert to 8-bit 4:2:0) →
`h264_v4l2m2m` (HW 8-bit encode). They are **offline / sub-real-time** and always will be
(CABAC is the serial wall); this log tracks attempts to shrink the CPU cost and — equally
useful — the ones that didn't pan out, with the reasons.

Companion docs: [`README.jellyfin-rpi.md`](README.jellyfin-rpi.md) (§ perf tables), [`CABAC-SIMD-analysis.md`](CABAC-SIMD-analysis.md) (the
entropy-decode deep dive). Measure with `perf -e task-clock` on the unstripped `ffmpeg_g`
(the installed `ffmpeg` is stripped); validate decode changes bit-exact with `-f framemd5`.

## Where the CPU goes (decode-only, single-thread, representative clips)

| symbol group | 10-bit 4:4:4 1080p | 12-bit 4:4:4 4K |
|---|---:|---:|
| **swscale** (`hscale16to15`, `yuv2planeX`) — downscale + convert | ~20% | ~23% |
| **`ff_hevc_hls_residual_coding`** (coeff parse + dequant) | ~12–14% | ~4% |
| **`get_cabac`** (context-coded arithmetic engine — serial) | ~9% | ~2% |
| MC (`put_hevc_*_hv`), loop filter, IDCT, dequant | remainder | remainder |
| `idct_*_12` (12-bit inverse transform) | n/a | **was ~21%, now ~0** (NEON, below) |

## Shipped

### 12-bit HEVC IDCT NEON — the real win
The `bit_depth==12` init wired only `idct_dc[]`, so the full inverse transform ran C;
`idct_32x32_12` alone was **~21%** of 12-bit decode. The IDCT asm is macro-parameterised,
so this was a clean instantiation at `bitdepth=12` + wiring `idct[]`.
- Files: `libavcodec/aarch64/hevcdsp_idct_neon.S`, `libavcodec/aarch64/hevcdsp_init_aarch64.c`.
- checkasm bit-exact; `idct_32x32_12` **7.53×** (69611→9242 cyc), 16x16 ~5×, 8x8 ~6.5×.
- Whole-decoder md5 == C; end-to-end 4K 4:4:4 12-bit transcode **0.18× → 0.22×**.

### CABAC `by22` bypass-batching — correct, small
Decode the per-subgroup bypass run (sign flags + `coeff_abs_level_remaining`) as one
reciprocal-multiply peek instead of per-bin. Adapted from rpi-ffmpeg (LGPL). Bit-exact on
16 clips; `residual_coding` self-time −10–14% relative on bypass-heavy content, but only
**~1–2% whole-decode** (near noise) — `get_cabac` dominates. Full write-up:
[`CABAC-SIMD-analysis.md` §8](CABAC-SIMD-analysis.md#8-by22-prototype--implemented--measured-bit-exact-modest-win). Files: `libavcodec/cabac.h`, `libavcodec/hevc/cabac.c`.

### CABAC CLZ renorm — correct, neutral-to-marginal
The per-bin renorm shift in the aarch64 `get_cabac` asm was a dependent table load;
replaced with `clz` (`norm_shift[x]==clz32(x)−23`). Bit-exact; `get_cabac` self-time
9.17%→8.64% (10-bit 4:4:4); wall-clock neutral (below ~1% noise). Kept as a correct,
load-removing, compounding change. [`CABAC-SIMD-analysis.md` §9a](CABAC-SIMD-analysis.md#9a-clz-renorm--shipped-bit-exact-neutral-to-marginal). File:
`libavcodec/aarch64/cabac.h`. (Dequant-hoist checked and dropped — dequant is ~0.8% of
decode, not a lever; [§9b](CABAC-SIMD-analysis.md#9b-dequant-hoist--not-pursued-the-4-opusfable-dispute-resolved). Remaining CABAC levers deferred as high-effort/marginal; [§9c](CABAC-SIMD-analysis.md#9c-remaining-levers--declined-the-clz-result-predicts-they-wont-help).)

## Dropped / negative results (with the why)

### SAO 10/12-bit NEON — profile no-go
Was the original target (only 8-bit SAO NEON exists; 10/12 fell back to C). A profile gate
first: on both a 10-bit and a 12-bit clip the SAO **filter** measured **<0.1%** of decode
(`sao_*_filter` never even sampled; only `hls_sao_param` header parsing showed, ~0.06%).
Not worth widening the kernels. Pivoted to the 12-bit IDCT instead. (Full CABAC/entropy
serial-wall analysis in [`CABAC-SIMD-analysis.md`](CABAC-SIMD-analysis.md).)

### ISP downscale offload for the SW path — measured no-go (for format-conversion cases)
Idea: since the target is 8-bit 4:2:0 anyway, do the 10/12→8-bit + 4:4:4/4:2:2→4:2:0
reduction on NEON, then let the **bcm2835 ISP** (`scale_v4l2m2m`) do the 720p downscale —
mirroring the shipped HW-path bridge (`vf_sand_to_yuv420p_drm`, `isp-experiments/`).

Blocked on two facts:
1. **The bcm2835 ISP ingests only 8-bit 4:2:0** (DRM_PRIME). It cannot take 10/12-bit or
   4:2:2/4:4:4. (The board's *other* 10-bit ISP is a camera RAW-Bayer path — irrelevant.)
2. **The downscale is not the separable expensive part.** Diagnostic (`perf`, decode-only,
   `scale=1280:720,format=yuv420p` vs `format=yuv420p` alone):

   | clip | scale+convert (current) | convert only (full-res) |
   |---|---:|---:|
   | 10-bit 4:4:4 1080p | swscale 20.2%, 0.288× | swscale 19.1%, 0.273× (slower) |
   | 12-bit 4:4:4 4K | swscale 22.5%, 0.0879× | swscale **26.9%**, 0.084× (slower) |

   Removing the geometric resize did **not** cut swscale cost — it stayed flat / rose, and
   overall got slower. The `4:4:4→4:2:0` chroma step is itself a horizontal `hscale`, so
   `ff_hscale16to15` runs regardless; the real cost is the **full-res 16-bit input read +
   chroma resampling + bit reduction**, which is mandatory and is **exactly what the ISP
   can't do**. Producing full-res 8-bit 4:2:0 for the ISP is ≥ the current fused cost, then
   ISP setup + a full-res dma-buf of extra memory traffic on a bandwidth-bound Pi on top ⇒
   strictly worse. swscale already does the smart thing (resize at high bit-depth first,
   then a cheap 720p convert) and is already NEON.

**The exception — where the ISP could still be worth it (unbuilt, needs pipeline changes).**
The SW path is entered for **two distinct reasons**, and they differ:
- **format** (4:2:2 / 4:4:4 / 12-bit) → mandatory CPU conversion → ISP can't help (above).
- **resolution only** (>4K but already **8-bit 4:2:0**, SW-decoded solely because rpivid
  caps at 4K) → **no format conversion needed**; the decoder already emits 8-bit 4:2:0, so
  the swscale cost there is *pure resize*, and the ISP **could** ingest it and offload the
  downscale (the 5.7K/8K 8-bit clips in the corpus are this case).

  What it needs: route the SW-decoded 8-bit 4:2:0 frame into a DRM_PRIME dma-buf and hand
  the resize to the ISP only on this branch — i.e. tweak *where in the pipeline* the
  hand-off happens, conditioned on the decoded pixel format. Cost of the frame→dma-buf
  import (SW frames aren't dma-bufs) vs the resize it saves is **unmeasured** — the open
  question if this is revisited. Not pursued yet.

## Current standing
The format-driven SW cases (4:2:2/4:4:4/12-bit) are near their practical floor on this
board: decode transform/entropy levers are shipped or exhausted, the scaler is irreducible
here (no HW ingests the source format), and encode is already HW. The one live lever is the
**resolution-only (>4K 8-bit 4:2:0) ISP-resize offload** above — conditional and unmeasured.
Otherwise these formats remain batch/offline on the Pi 4, as [`README.jellyfin-rpi.md`](README.jellyfin-rpi.md) states.

## Parked threads (investigate later)

These are not pursued yet; recorded so they aren't lost or re-derived.

### A. Opt-in lossy "fast decode" (behind a CLI flag)
Trade decode accuracy for speed *deliberately*, since the pipeline already discards
accuracy (3:1/1.5:1 downscale + lossy 3 Mbit re-encode). Candidate approximations:
truncated / low-frequency-only inverse transform; decoding at reduced bit depth before MC;
early chroma subsampling to 4:2:0 before MC; skipping in-loop filters.
- **Why it might be acceptable:** the 3:1 downscale is itself a low-pass filter that
  removes the high-frequency band where truncated-IDCT / dropped-LSB error lives, so much
  of the error may never reach the encoder. This is the crux — needs verification.
- **The real risk — drift, not per-frame error:** these are non-conformant, so the
  decoder's reference frames diverge from the encoder's and error **accumulates over
  inter-prediction chains** (worst at end-of-GOP, reset at each I/IDR). The artifact to
  look for is *progressive* degradation within a GOP.
- **Free first probe (no code):** FFmpeg's existing `-skip_loop_filter all|nonref|bidir`
  is exactly a "faster but incorrect" knob (skips deblocking) — measure speed and inspect
  artifacts after downscale+re-encode before writing any custom truncation.
  (`-lowres` DCT-downscale decoding — the cleanest form of the idea — is **not** wired for
  HEVC in FFmpeg, only older codecs, so that route is custom work.)
- **Evaluation:** VMAF/SSIM of the final 720p output vs the correct transcode across a few
  clips, plus visual inspection for GOP-progressive drift. Must be **opt-in via a flag** —
  never the default (it produces non-conformant output).

### B. Decode/convert fusion (bandwidth, not accuracy)
swscale currently re-reads the full-res reconstructed frame **cold** from DRAM (~50 MB/frame
for 4K 4:4:4 12-bit) to downscale+convert. The MC-reference frame write to DRAM is
unavoidable (references are consumed at native res/depth by later frames — the full frame
*must* be materialized), but the *cold re-read* could be avoided by converting each region
into the 720p 8-bit 4:2:0 output while it is still cache-warm from reconstruction
("tapping the reconstruction write stream"). Caveats: deep surgery breaking FFmpeg's
decode↔filter separation (CTU-row completion hook, downscale vertical-filter window,
deblock/SAO cross-boundary lag, non-CTU-aligned output); and it saves only swscale's
**memory-read** component, not the polyphase `hscale` **compute**. Cheapest gate before any
work: measure the memory-read vs compute split of swscale's ~23% (perf bus/cache-stall
events + the `v3d-experiments/membw` probe). Unmeasured.
