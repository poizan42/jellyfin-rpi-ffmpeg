# FFmpeg — Jellyfin / Raspberry Pi 4 fork

A fork of Jellyfin's FFmpeg (8.1.2 base) whose purpose is **real-time hardware
transcoding of various source formats to H.264, with downscaling, on the Raspberry Pi 4**.

The workload it targets: take a high-resolution source (primarily HEVC) and produce a
lower-resolution **8-bit H.264** stream using the Pi 4's fixed-function silicon end to end —
**rpivid** HEVC hardware decode → **NEON** pixel-format unpack → **bcm2835 ISP** hardware
downscale → **bcm2835** H.264 hardware encode — kept **zero-copy (DRM_PRIME)** the whole way,
so the CPU only touches the one step no hardware block can do (the 10-bit column-tiled
"SAND" → planar unpack).

Full design, build instructions and usage: **[README.jellyfin-rpi.md](README.jellyfin-rpi.md)**.

## Current status

| source → H.264 720p (Pi 4B) | result |
|---|---|
| 10-bit HEVC **SDR** 1080p | **~2.0–2.7× real-time** — the tuned sweet spot |
| 10-bit HEVC **SDR** 4K (2160p / scope) | **~1.64× real-time**, or **~2.11×** when `out=half` does the first 2:1 (slice-threaded NEON unpack + prefetch + map-cache + ISP scale) |
| 8-bit HEVC SDR | works (same bridge path) |
| 10-bit HEVC **HDR10** 4K | **~1.27× real-time with correct colour** (`tm=fast`) — single-pass NEON HDR→SDR tone-map (PQ/BT.2020→BT.709). The higher-quality `tm=accurate` tier (3D-LUT, matches zscale) is now real-time too at ~1.02×. `tm=none` is ~1.23× but wrong colour (a truncation cannot fit HDR into SDR) — it exists as the unpack-only baseline. HLG sources handled too. |
| Dolby Vision **profile 5** 4K | **supported** — reconstructed from the per-frame RPU to HDR10, then tone-mapped (`AV_FRAME_DATA_DOVI_METADATA` → per-scene 3D-LUT + NEON tetrahedral). `tm=fast` **~1.03× (real-time)**, `tm=veryfast` ~1.11× (headroom; ordered-dithered nearest chroma — de-banded), default full-3D path ~0.69×. **At 4K→1080p, `out=half` (fused 2×2 downscale, no ISP scale) makes even `tm=accurate` real-time (~1.16–1.32×)**, and at a 720p target `out=half` reaches **~1.46× on DV P5 / ~1.53× on HDR10**. |
| H.264 / VP9 / AV1 sources | outside this pipeline (rpivid decode is HEVC-only) |

**The 10-bit HEVC → 8-bit H.264 downscale pipeline is proven to run above real-time through 4K,
for SDR, HDR10, and Dolby Vision profile 5** — HDR10 and DV P5 get a single-pass NEON tone-map
(`tm=fast`) that keeps them real-time with correct colour (DV P5 is reconstructed from its RPU
first). Higher-quality tone-map tiers (`tm=accurate`, and the default full-3D DV path) trade speed
for a closer match to the zscale reference (~1.02× / ~0.69×), and `tm=veryfast` gives DV P5 extra
headroom at coarser chroma. Validated on a real library (see the status doc); on a 1080p transcode
the pipeline is limited by the single-threaded rpivid decode thread, not the CPU.

All 4K figures were re-measured 2026-08-16 on kernel 6.18.44 and are 5–9% above the previous ones,
after fixing the bridge filter's input mmap cache: it required a frame to be described by exactly
one dma-buf object, so it silently disabled itself whenever the decoder split luma and chroma into
two — always on a 6.18 kernel, sometimes on 6.1.

## Documentation (this fork)

- **[README.jellyfin-rpi.md](README.jellyfin-rpi.md)** — full design, build, usage, the
  supported-input matrix, and the performance tables (HW and software-decode paths).
- **[rpi-notes/sw-decode/optimization.md](rpi-notes/sw-decode/optimization.md)** — optimization log for the
  software-decode path (the 4:2:2 / 4:4:4 / 12-bit / >4K formats hardware can't do):
  shipped wins (12-bit IDCT NEON, CABAC), measured-and-dropped experiments with the reasons,
  and parked future threads.
- **[rpi-notes/sw-decode/cabac-simd.md](rpi-notes/sw-decode/cabac-simd.md)** — deep dive on whether HEVC CABAC /
  entropy decode can be SIMD-ised, and the micro-optimisations that came out of it.
- **[rpi-notes/pipeline/input-support.md](rpi-notes/pipeline/input-support.md)** — roadmap for widening the
  input formats the transcode path accepts.
- **[rpi-notes/pipeline/tonemap.md](rpi-notes/pipeline/tonemap.md)** — deferred follow-ups for the SAND→YU12
  HDR tone-map filter.

---

# Upstream FFmpeg README

FFmpeg README
=============

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides means to alter decoded audio and video through a directed graph of connected filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
