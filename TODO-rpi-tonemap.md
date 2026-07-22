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

## Other
- 32-bit `arm/rpi_sand_neon.S` parity for the fast-tier tone LUT (aarch64 done first; Pi 4 is 64-bit).
- Dolby Vision profile 5 is out of scope (needs DV RPU processing; generic PQ tone-map can't fix IPT-PQ).
