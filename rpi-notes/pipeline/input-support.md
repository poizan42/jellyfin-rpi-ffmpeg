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

**Perf is software-HEVC-decode-bound — and (measured on real multi-frame clips) below real-time even
at 1080p:**
- **1080p 4:4:4 10-bit: ~0.48× (12 fps)** end-to-end; decode-only ~0.63× (15 fps). So even 1080p is
  NOT live — the swscale downconvert + resize + HW encode add ~20%, but the sw HEVC decode ceiling
  (15 fps) is already sub-real-time for 10-bit 4:4:4 (2× the chroma of 4:2:0). (`hevc_2K24P_rext_10bit_444_yuv444p10le_1`, 24p.)
- **4K: ~0.05–0.10×** — 4K 4:2:2 10-bit ~0.05× (≈3 fps, both `…eos_r5c` and `hevc_4k50P`), 4K 12-bit
  4:4:4 ~0.10× (2.5 fps, `hevc_4K24P_rext_12bit_444_yuv444p12le_1`). Deeply offline.
- The downconvert/scale/encode are only ~15–20% of the cost; **sw HEVC decode is the wall, so no
  kernel work reaches real-time** — a *free* downconvert still caps at the ~15 fps decode ceiling
  at 1080p. The only lever is faster sw HEVC decode (upstream libavcodec / threading), same as §2.
- Not measured: low-res 8-bit RExt (lighter — may approach real-time) — the corpus's only such clips
  are 1-frame test patterns, so **don't assume any of these are live**.

**Net: no kernels needed, and effectively offline/batch on the Pi 4 at real resolutions.** Deliverables
shrink to (a) wrapper routing for non-4:2:0/>10-bit HEVC → software path (correctness is free there);
(b) treat the whole group as offline (merge with §2); real-time is not on offer. **The routing rule
(a) is now specified** — see `README.jellyfin-rpi.md` §Path selection + the reference implementation
`tools/rpi-transcode-path.sh` (validated: routes all 59 corpus clips to the path that actually works). If an HDR non-4:2:0
clip appears (e.g. the corpus's `hevc_4k24P_rext_12bit_444_pq`, SMPTE2084 4:4:4 — 4K, so offline
anyway), the software path must add a CPU tone-map (`zscale`/libplacebo; note the lean production
build has no `zscale`) or it comes out washed-out like `tm=none`.

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

## 3. Accelerating the software-HEVC-decode wall — HW CABAC offload: RULED OUT for now

Both §1 (non-4:2:0/12-bit → offline) and §2 (>4K → offline) bottom out on the same wall: **software
HEVC decode on the A72**, whose slowest, inherently-serial part is CABAC entropy decode. Idea
considered: offload that entropy stage to one of the Pi's fixed-function CABAC engines. **Ruled out
under the current constraints** (no firmware modification / no custom VPU code):
- **Argon (HEVC) CABAC** is ARM-reachable (rpivid drives the `hevc` MMIO block directly) but is *fused*
  into a monolithic 4:2:0-only fixed-function pipeline — no standalone "entropy-only" tap, and it
  rejects non-4:2:0 outright. Useless as a reusable primitive.
- **H.264 CABAC** is the closer thing to a standalone entropy coprocessor (128-bit command ring +
  doorbell), but it is **not ARM-visible** — it sits behind the VPU firmware / VCHIQ mailbox — and its
  context models + command semantics are H.264-specific. Not reachable, and not obviously HEVC-usable.
- Even if reachable, a per-symbol ARM↔coprocessor round-trip would likely cost more than the
  arithmetic saved (CABAC decodes ~billions of bins/frame; these engines are meant to be sequenced by
  adjacent silicon / on-die firmware, not driven symbol-by-symbol from the CPU).

**Revisit only if/when the VPU-reversing strand yields the ability to run custom VPU code.** In that
world one genuinely open question is worth an experiment: **can the H.264 CABAC command-queue engine be
fed H.265 and produce anything useful?** The arithmetic range-decode *core* is shared between the two
codecs even though context modeling / binarization differ — so a hybrid (engine does raw bin decoding,
ARM/VPU does HEVC context selection) is at least conceivable. Speculative; strictly gated on the
custom-VPU-code capability existing, and on driving the engine outside the firmware's H.264 path.
See the parent research repo's `VideoCore-Codec-Architecture.md` §2 (H.264 CABAC command queue,
registers `0x7f000b*`/`0x7f0027*`, doorbell `0x7f002714`) and §3 (the Argon HEVC block) for the
reverse-engineered mechanics.

---

## 4. Software HEVC decode — profiled NEON gaps (the productive lever for §1/§2)

Unlike the CABAC-offload dead-end (§3), the software HEVC decoder itself has large, straightforward
NEON gaps — and closing them helps every offline case (all 10-bit). **Profiled** (`perf` on `ffmpeg_g`,
4K 4:2:2 10-bit, 150 frames; aggregated across frame-threads):

| function | share | status |
|---|---:|---|
| `put_uni_luma_hv_10` (`h26x/h2656_inter_template.c`) | **26.5%** | **C — no 10-bit NEON** |
| `put_uni_chroma_hv_10` | **17.5%** | **C — no 10-bit NEON** |
| `ff_hevc_hls_residual_coding` | 15.8% | CABAC (scalar; not SIMD-able) |
| `get_cabac` | 8.7% | CABAC (scalar) |
| idct / deblock / add_res `*_10_neon` | ~6% | already NEON |

**Root cause:** the H.26x inter-prediction NEON (`aarch64/h26x/qpel_neon.S`, `epel_neon.S`) and the old
`hevcdsp` MC NEON are **8-bit only** — every symbol is `..._8_neon`, no `_10`. So on 10-bit content
(all our software-decode cases) motion comp runs entirely in the C template (~44%). HEVC **intra
prediction has no NEON at any depth** (no `pred_planar/dc/angular` in the aarch64 init; matters for
all-intra content). SAO NEON is 8-bit-only too.

**NEON payoff scale on this A72** (checkasm `hevc_add_res`, which *has* 10-bit NEON): **8–12× vs C**
(32×32 10-bit 5279→544 cyc = 9.7×). MC is a 2-pass 8-tap filter so expect a more modest ~3–6×, but on
a ~44% chunk that still projects to **~1.4–1.6× faster 10-bit decode**. CABAC (~25%) is the hard floor
— inherently serial, no NEON.

**Ranked opportunities (all software-decode only — the HW 4:2:0 path is unaffected):**
1. **10-bit H.26x MC NEON** (qpel luma + epel chroma; `h`/`v`/`hv` and `put`/`uni`/`bi`/`*_w`) — the top
   lever, ~44%+ of decode. **Checked upstream — not a backport: `aarch64/h26x/qpel_neon.S` + `epel_neon.S`
   are 8-bit-only (`_8_neon`, zero `_10`/`_12`) at every version — n7.1.5, n8.0.3, AND current master.**
   So it's a from-scratch `.S` (widen the 8-bit kernels to 16-bit loads / wider intermediates + `hv`
   32-bit intermediate), and since the `h26x` template is shared by HEVC *and* VVC it's genuinely
   contribution-worthy — target upstream FFmpeg, not just this fork.

   **SHIPPED (2026-07-24) — the full non-weighted 10-bit MC family (uni + put + bi), all widths.**
   Added width-generic 10-bit kernels in `aarch64/h26x/qpel_neon.S` (luma, 8-tap) / `epel_neon.S`
   (chroma, 4-tap), gated `bit_depth==10` (the dsp table passes block width as an arg → one pointer per
   (family, my, mx) covers every size slot 4/6/8/12/16/24/32/48/64):
   - **uni** `h`/`v`/`hv` → pixel out, fused `sqrshrun #6` (h/v) / `#10` (hv) `+ umin 1023`;
   - **put** `h`/`v`/`hv` → int16 out (the bi-pred ref0 intermediate), `sqshrn #2`/`#6`;
   - **bi** `pixels`/`h`/`v`/`hv` → reads src2, combines `clip((p+src2+16)>>5)` via s32 `saddl`+`sqrshrun #5`.
   All fusions bit-exact to the two-stage C shifts. H fills a 128B-stride int16 scratch in 8-col blocks;
   V walks the width in 8-col blocks (shared `calc_all`/`calc_all4` ring) + a partial tail for 4/6/12.
   **checkasm `hevc_pel` bit-exact vs C for every width x {pixels,h,v,hv} x {uni,put,bi} x {luma,chroma}
   at depth 10 (coverage proven by shift perturbation); full-decoder output md5-identical** to the C path.

   **Measured (4K 4:2:2 10-bit, this A72; full C-MC vs full NEON-MC, thermal-fair interleaved A/B —
   the Pi 4 throttles, so only an interleaved comparison is trustworthy; utime is *not* throttle-independent):**

   | metric | full C MC | full NEON MC | ratio |
   |---|---:|---:|---:|
   | wall (150f) | ~26–27 s | ~20–22 s | **~1.25–1.3×** |
   | CPU utime (150f) | ~92–107 s | ~70–79 s | **~1.3×** |

   → **~1.3× faster 10-bit software decode.** Per-kernel, the hv apply roughly halves in the profile
   (`qpel_uni_hv` 26.5→14.0% self, `epel_uni_hv` 17.5→10.2%, i.e. ~1.9× on the kernel); the end-to-end
   gain is Amdahl-capped by **CABAC**, now the top cost (`ff_hevc_hls_residual_coding` 21.9% +
   `get_cabac` 12.4% self — the serial floor of §3). Note: an earlier Phase-C figure of “~1.4× wall /
   ~1.6× CPU” was from a *non-interleaved* A/B (the C build ran second, warm/throttled) and overstated
   it; ~1.3× interleaved is the honest number. The RExt corpus is **uni-predicted**, so `put`/`bi` add
   ~no local speedup here — they are coverage for B-frame content and upstream completeness (validated
   bit-exact).

   **COMPLETE (2026-07-24): the entire HEVC motion-comp NEON, 10- AND 12-bit.** Beyond the profiled
   `uni_hv`, the full family is now NEON, all checkasm bit-exact (839 `hevc_pel` cases pass at depth
   8/10/12): **uni** (h/v/hv), **put** (h/v/hv, int16 out — the bi ref0 intermediate), **bi** (pixels/
   h/v/hv, +src2 combine), **uni_w** and **bi_w** (weighted uni/bi — variable runtime shift via
   `dup`+`sqrshl`/`sshl`, weights/offsets from the extra stack args), each for luma (qpel, 8-tap) and
   chroma (epel, 4-tap), every width. 12-bit is derived from 10-bit by constant substitution (h-shift
   #4, uni-hv v #8, bi #3, bi_w log2Wd base +2, ox<<4, pixels<<2, clip 4095); the two paths could be a
   single BD-parameterised macro set — a worthwhile upstream cleanup, deferred to keep the shipped
   10-bit untouched. Measured decode win is the ~1.3× above (uni-predicted corpus); `put`/`bi`/`*_w`
   are coverage for B-frame / weighted content and upstream completeness. 8-bit path unchanged.
   Committed to the fork (`jellyfin-rpi`); clean for upstream submission (benefits all aarch64,
   HEVC + VVC via the shared `h26x` template).
2. **HEVC intra-prediction NEON** (planar/DC/angular, `hevc/pred_template.c`) — none exists; dominant for
   all-intra clips (`hevc_all_i`, RExt test set). Also likely worth upstreaming.
3. **SAO 10-bit NEON** — 8-bit only today; small.
4. **CABAC** — not SIMD-able; only scalar micro-opt (branch layout), low ceiling. Leave it.

Note this is really **upstream FFmpeg work** (benefits all aarch64 users), so prefer backport/contribute
over a private fork patch. Tooling on this box: `perf` (paranoid must be ≤1 — `sudo sysctl` it),
`checkasm --test=hevc_pel --bench` for per-kernel C-vs-NEON, `valgrind --tool=callgrind` for whole-decode
attribution incl. CABAC. Profile `ffmpeg_g` (unstripped) for symbols.

## Not in scope (hard limits, for the record)
- **Non-HEVC codecs** (H.264 / VP9 / AV1) — rpivid is HEVC-only, so none of them ever produce a
  SAND frame; out of the HW-decode pipeline entirely. The encode + ISP tail still applies (software
  decode → `sand_to_yuv420p_drm` SW-input → `scale_v4l2m2m` → `h264_v4l2m2m`), so they transcode —
  just software-decode-bound.
  - **H.264 specifically:** the Pi 4 *does* have a separate legacy H.264 hardware decoder
    (`bcm2835-codec`, `/dev/video10`), but its coded-input format range is **`H264 32×32 – 1920×1920`**
    (verified: `v4l2-ctl -d /dev/video10 --list-formats-out-ext`). So it can HW-decode **≤1080p** H.264
    but **not 4K** — 3840 exceeds 1920 on both axes. 4K H.264 therefore falls to **software** decode.
  - **4K H.264 is not real-time and can't be made so.** Measured on this Pi 4B (`ffmpeg -threads 0`,
    20 s steady-state, `samples/kodi/high-bitrate/{jellyfish,test-videos}/`):

    | source | decode-only ceiling | full → 720p | full → 1080p |
    |---|---:|---:|---:|
    | 4K30 H.264 High @120 Mbit (jellyfish) | **0.50×** (15 fps) | 0.42× | 0.40× |
    | 4K60 H.264 High @40 Mbit (Test AVC) | **0.47×** (28 fps) | 0.34× | — |

    (Decode measured with `-threads 0` = auto.) The wall is **decode-only at ~0.5×** — before any
    scale/encode — so no amount of tuning our HW scale/encode side reaches 1.0×. The decode itself is
    unmovable, and **threading plateaus below real-time**: ffmpeg's H.264 decoder is already NEON +
    frame-threaded, but on this 4-core Pi it tops out at ~3–3.6 cores busy and won't saturate all four
    — `-threads 0` → 302 % CPU / 0.44×, explicit `-threads 4` → 356 % CPU / **0.52×**, `-threads 1` →
    0.17× — because frame-dependency + per-slice entropy decode serialize it. There is also no ≥1080p
    H.264 HW block to offload to, and inter-frame prediction forbids frame-skipping. **4K H.264 → transcode is offline/batch only; the
    only real-time answer is client direct-play (no transcode).** In practice 4K distribution is almost
    all HEVC, so this is rare. (rpivid HEVC on `/dev/video19` goes to 4K — the limit is codec-specific,
    not a general 4K-decode limit.)
- **Output beyond 8-bit 4:2:0 H.264 ≤1080p** — fixed by the bcm2835 encoder (level 4.0, one stream).
- **DV profile 7 dual-layer** — expected to decode its HDR10 base only (EL/RPU ignored); untested, no
  P7 sample in the corpus. Add a P7 sample and verify if it ever matters.
