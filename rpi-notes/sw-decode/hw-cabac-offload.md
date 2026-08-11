# Offloading entropy decode to the VideoCore's hardware CABAC — RULED OUT

**Status:** negative result, measured (2026-08-12). Applies to the **software 4K H.264**
decode path — the one input class the Pi 4 can neither hardware-decode (the firmware
`bcm2835-codec` H.264 block is Level-4.0 / 1080p; see
[`../pipeline/input-support.md`](../pipeline/input-support.md) "Not in scope") nor
software-decode in real time (~0.5×). The idea: the VideoCore has a fixed-function CABAC
entropy engine — drive it to take the serial entropy stage off the A72s. **It does not get
4K H.264 to real-time, and the reason is Amdahl, before any of the (also fatal) plumbing.**

## What we already know about the engine (it's reversed, not a black box)

The H.264 CABAC engine is documented in the research repo's `VideoCore-Codec-Architecture.md`
(§1.3–1.6, §2) — this note builds on that, so the feasibility call rests on reversed facts,
not speculation:

- **The entropy decode is in silicon**, a **command-queue coprocessor** in the VPU
  peripheral regions `0x7f000b*` / `0x7f0027*`: clock-gated via `GCKE_A_CABAC` (bit 4 of
  `0x7f500e80`), engine-enabled at `0x7f00272c`, fed 128-bit decode commands through a
  64-entry ring (`base 0x7f002700`, doorbell `0x7f002714`), results collected from a **2 KB
  output slot** by `cabac_complete`, with a range-coder read pointer `rc_rd_ptr`.
- **It is driven by the VPU firmware, not the ARM.** The firmware parses NAL/SPS/PPS in VPU
  software, orchestrates the command ring, and runs **reconstruction (MC / transform /
  deblock) on the VPU vector unit**. The ARM only ships buffers in via the MMAL/VCHIQ
  mailbox (`bcm2835-codec`).
- **No ARM driver reaches it; it's driven only via the mailbox today.** Unlike the HEVC/Argon
  block (an ARM-MMIO peripheral at `0x7eb00000`, in the device tree, driven directly from the
  ARM by `rpivid`), the H.264 hardware has no device-tree node and is reached only via the
  MMAL/VCHIQ mailbox to the VPU firmware. (Structural symmetry noted in §3 of that doc: both
  are command/bit-FIFO-fed fixed-function CABAC engines — the difference is *who drives them*,
  ARM vs VPU firmware.)

So "could a custom device tree expose it?" — **unknown, and not how it's driven today.** Its
registers sit in the VPU peripheral space (`0x7f00*`); whether the ARM can be made to address
that region at all is **not established** — we have *not* reversed how (or whether) the VPU
maps its peripheral space into an ARM-visible physical window, so this is an open question, not
a proven "no." A DT-overlay + driver route therefore can't be assumed possible *or* impossible;
at minimum it would need (1) establishing ARM addressability of `0x7f00*`, then (2) a driver
reimplementing the firmware's command-ring orchestration. The only route we *know* works is
**from the VPU** — extending the firmware, or `EXECUTE_CODE` custom VPU code (proven in
`~/rpi-hacking`, but **root**, and it can **wedge the board** to a hard reset). The verdict
below does not hinge on this: even with the engine perfectly in hand, Amdahl rules it out.

## Why it can't reach real-time anyway — Amdahl

Software 4K H.264 **decode-only** tops out at **~0.5×** real-time on this Pi 4B (`-threads 0`;
plateaus ~3.6 cores / 0.52× even at `-threads 4`; single-thread 0.17×). A CABAC offload only
removes the **entropy** slice; MC, deblocking, IDCT and reconstruction stay on the CPU.

`perf` self-time on the fork's `ffmpeg_g`, 4K H.264 decode-only, summed across frame threads
(entropy = `get_cabac` + `decode_cabac_residual*` + `ff_h264_decode_mb_cabac` + sub-cutoff):

| sample | decode-only | **entropy share** | rest (MC / deblock / IDCT / caches) |
|---|---:|---:|---|
| jellyfish 4K30 **@120 Mbit** (residual-heavy, synthetic) | ~0.44–0.50× | **~38–42 %** | ~58 % |
| Test Jellyfin AVC 4K60 **@40 Mbit** (realistic) | ~0.47× | **~18–22 %** | ~78 % (deblock ~14 %, MC+prefetch ~20 %) |

Grant a **free** offload (zero cost, perfect overlap — impossible best case): new speed =
decode-only / (1 − entropy_share):

- **120 Mbit:** 0.44 / (1 − 0.40) = **~0.73×**
- **40 Mbit:** 0.47 / (1 − 0.20) = **~0.59×**

**Neither reaches 1.0×.** At realistic bitrates entropy is only ~1/5 of the work; the
majority is deblock + motion-comp + reconstruction, all still on the A72s. The high-bitrate
synthetic clip inflates CABAC to ~40 %, and even *free* it lands at ~0.73×.

## And it wouldn't be free — the reversed architecture makes it worse

Every property we reversed points the wrong way, on top of the Amdahl ceiling:

1. **The output stays VPU-side → read-back is the V3D wall again.** Decoded syntax/coeffs
   land in the engine's **2 KB VPU-memory slot** per command, collected by VPU firmware.
   Using them for CPU reconstruction means ferrying residuals + modes + MVs (4K ≈ 32 400
   MB/frame) VPU→ARM every macroblock — a large scattered transfer, the exact
   access-pattern wall the V3D offload already lost to. Plausibly costs more than the 20–40 %
   it saves.
2. **Not separable, and it's welded to firmware.** The engine is fed by firmware that does
   the NAL/context orchestration and consumes its output for VPU-vector reconstruction. There
   is no exposed "entropy-only, hand results to the CPU" path; you'd re-implement the
   firmware's command-ring orchestration in your own VPU code just to drive it.
3. **1080p-class silicon.** It's part of the "dec3" Level-4.0 block: MaxFS 8192 MBs
   (~1920×1088), ≤25 Mbps, and the firmware NAL parser rejects `image too large` above
   **2048×1920**. The engine is designed around ≤1080p context/line state; there is no
   evidence it handles 4K, and good reason to doubt it — 4K is exactly why we're on the
   software path. (Pi 5 confirms the provenance: BCM2712 dropped the whole legacy H.264
   firmware codec — H.264 there is pure ARM software decode — while keeping the ARM-driven
   HEVC/Argon block.)

## Verdict

**Do not pursue HW CABAC offload for 4K H.264.** A *free* offload is Amdahl-capped at
~0.6–0.73×; then the VPU-side read-back, the non-separable firmware coupling, and the
1080p-class engine each independently sink it — and the only route we *know* reaches it is
root-only, board-wedging VPU code (whether an ARM-driven route is even possible is
unestablished, but moot given the above). 4K H.264 stays **offline / batch, or client
direct-play** (in practice 4K distribution is almost all HEVC, which *does* hardware-decode to
4K via `rpivid`).

This is the H.264 companion to the HEVC-side ruling in
[`../pipeline/input-support.md` §3](../pipeline/input-support.md#3-accelerating-the-software-hevc-decode-wall--hw-cabac-offload-ruled-out-for-now);
for why the *software* CABAC engine is also near-un-SIMD-able, see
[`cabac-simd.md`](cabac-simd.md). Engine internals: research repo `VideoCore-Codec-Architecture.md` §2.
