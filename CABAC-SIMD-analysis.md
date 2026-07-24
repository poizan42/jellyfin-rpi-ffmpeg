# CABAC / entropy-decode SIMD feasibility — HEVC software decode on Cortex-A72

**Status:** analysis only, no code written yet. Percentages are educated estimates
until measured. Target: Raspberry Pi 4, Cortex-A72, **aarch64 NEON only** (128-bit,
**no SVE, no gather**). Workload: the software HEVC decode path for the formats the
hardware decoder can't do (4:2:2 / 4:4:4, 12-bit, >4K).

This note stress-tests the long-standing assumption in our docs that **"CABAC is the
serial wall — inherently un-SIMD-able."** Two independent code-grounded audits were run
(one Opus 4.8, one Fable 5). They converged on the core result and diverged usefully on
the exploitable edges; this file records both.

Profile context (10-bit clip, `perf` on `ffmpeg_g`, single-thread):
`get_cabac` ~6% and `ff_hevc_hls_residual_coding` ~10% of decode self-time
(CABAC + residual coding ≈ 16–25% total).

Code references are `file:line` in `libavcodec/`.

---

## 1. Headline verdict

The assumption is **correct for context-coded bins, but for a sharper reason than
usually stated, and it over-generalizes.**

- FFmpeg's per-bin engine is **already branchless** (arithmetic sign-masks + `csel`,
  including the aarch64 hand-asm). The classic "CABAC is too branchy to vectorize"
  motivation is already spent.
- The true per-bin cost is **load-use latency**, not branches or ALU throughput:
  2–3 dependent table loads (~4-cycle L1 each) chained through the ALU ≈ **~14–16
  cycles of latency-bound serial work per bin**, with the 3-wide OoO core mostly idle.
- The realistic ceiling from everything below is **~3–6% off total decode** — a door in
  the wall, not a demolition.

---

## 2. The actual recurrences (from the code, not folklore)

Three nested feedback loops, of very different hardness.

### Loop A — arithmetic interval, per bin — `cabac_functions.h:116-137`
```
118  RangeLPS = ff_h264_lps_range[2*(range & 0xC0) + s]   // table load, idx = f(range, state)
121  range  -= RangeLPS
122  lps_mask = ((range<<17) - low) >> 31                 // MPS/LPS decision — branchless sign mask
124  low   -= (range<<17) & lps_mask
125  range += (RangeLPS - range) & lps_mask
127  s     ^= lps_mask
128  *state = (ff_h264_mlps_state+128)[s]                 // 2nd dependent load + STORE (adaptation)
131  shift  = ff_h264_norm_shift[range]                   // 3rd dependent load (renorm amount)
132  range <<= shift; low <<= shift                       // renormalize (variable shift)
134  if(!(low & CABAC_MASK)) refill2(c)                   // ~1-in-16-bin byte refill (only branch left)
```
- Loop-carried state: `{range, low, bytestream, *state}`. `low` folds the 16-bit
  look-ahead window into a 32-bit register (refill at `cabac_functions.h:89-113`); it is
  the **unbounded** part of the state — the reason no bounded mid-stream restart point
  exists.
- The aarch64 engine asm is a direct scalar transliteration of this
  (`aarch64/cabac.h`, `get_cabac_inline`, reached from HEVC's `GET_CABAC` macro at
  `hevc/cabac.c:514`). **Note:** there *is* aarch64 cabac asm — but only the regular-bin
  engine; **bypass has no aarch64 asm** and falls through to generic C
  (`cabac_functions.h:149`).

### Loop B — per-context probability adaptation — `cabac_functions.h:128`
`*state = mlps_state[s ^ lps_mask]`. Chains only bins that **share a context**; different
contexts have independent state bytes.

### Loop C — control flow / context selection (HEVC layer, `hevc/cabac.c`)
The decoded bit decides which syntax element (hence context, hence table row) comes next.
**Crucial finding — Loop C is absent in the hot loops:**

| Syntax element | Context selection | In the just-decoded feedback loop? |
|---|---|---|
| `significant_coeff_flag` (dominant bin) | `ctx_idx_map[(y_c<<2)+x_c] + offset` (`:915`, loop `:1266-1274`) | **No** — position-only; the whole 15-bin context sequence is known *before* the loop. |
| `significant_coeff_group_flag` | neighbor CGs (`:1194-1202`) | From a prior group iteration — resolved before this group's bins. |
| `coeff_abs_level_greater1_flag` | `(ctx_set<<2)+greater1_ctx` (`:1334-1345`) | **Yes** — true bin-to-bin feedback (but small). |
| `ctx_set` per subset | `+1` if prev subset ended `greater1_ctx==0` (`:1329`) | Yes, subset-to-subset. |
| `c_rice_param` | adapts to decoded level magnitude (`:1375-1400`) | Yes — but drives bypass suffix *length*, not a context. |

So the context-feedback recurrence reduces to the small `greater1_ctx`/`ctx_set` chain.
**The dominant significance bins are NOT in the feedback loop** — the most-cited
justification for "un-SIMD-able" does not apply to the hottest loop.

---

## 3. The angles, judged against that structure

### Algebra (parallel scan / monoid composition)
Per-bin decode is function composition, which **is** associative — so "not associative"
is the wrong diagnosis. The blocker is **compact representability**: a context-coded
bin's map on `(low, range)` is piecewise-affine (2 pieces: MPS/LPS), with piece boundary
and LPS slope set by the nonlinear `lps_range` lookup + variable renorm. Composing k maps
gives up to **2^k pieces that do not collapse** into any small parameterized family. The
scan "combine" operator exists but its operands grow exponentially.
**Speculation and parallel-scan are the same idea in two costumes and die of the same
disease.** Additionally Loop C means the operator *sequence* isn't known up front (except
in the sig-map loop, where it is).

### Branchless / lane speculation
- Branchless is already done. The unexplored *opposite*: branchlessness welds the
  dependency chain shut, whereas a **predicted branch** on the MPS/LPS decision lets the
  OoO core speculate past it for free (MPS renorm needs no `norm_shift` load). For skewed
  contexts (CBF/skip/gt1, p(LPS)≈10–20%) expected mispredict cost may be < the chain
  latency removed; for p≈0.5 sig-coeff it loses. Hybrid branchy variant for skewed
  call-sites = a measurable micro-experiment, low-single-digit % at best.
- **Explicit SIMD-lane outcome speculation is dead** on NEON: state needs ≥26 bits ⇒
  ≤4 lanes/128-bit ⇒ ~2 bins lookahead; and no gather ⇒ per-lane `lps_range` lookups
  serialize. The real "lanes" on an A72 are its OoO execution ports, not NEON.
- **Scalar dual-preload pipelining of the sig-map loop survives** (because its context
  sequence is knowable in advance): preload bin n+1's state byte and *both* candidate
  `lps_range` entries while bin n resolves, then `csel`. `greater1_ctx ∈ {0..3}` ⇒ the
  four candidate context states are four *adjacent* bytes of `cabac_state` — one 32-bit
  load fetches all possible futures. A genuine, unexploited ILP reserve.

### Bypass bins — the exact, exploitable exception
`get_cabac_bypass` (`cabac_functions.h:149-163`) **never writes `range`** — it reads
`range<<17` only as a fixed comparison threshold. So across a bypass run `R = range<<17`
is **constant**, and each bin is `x → 2x − b·R`. The k-fold composition has the closed
form `x → 2^k·x − V·R`, `V = ⌊2^k·x / R⌋` — i.e. **k bypass bins in one integer division
/ `umulh` by a per-`range` reciprocal** (computed once per run; `range` only changes on
context-coded bins). This is where the algebra collapses cleanly: compactness breaks
*only* when the range-update table and context adaptation enter — both absent in bypass.

### Vectorize around the serial core
Much of `ff_hevc_hls_residual_coding`'s 10% is **not entropy decode**: per-coefficient
`int64` dequant/level-scaling (`hevc/cabac.c:1411-1434`) and cross-component add
(`:1483-1489`) are plain arithmetic done scalar, interleaved with parsing. Parse levels
serially into the compact array, then do **one NEON pass** for sign/dequant/saturate over
up to 16 coeffs, then scatter by scan LUT. Zero bitstream-path risk.

---

## 4. Tractable sub-problems (ranked)

1. **Multi-bit bypass via one division / reciprocal** — signs (`hevc/cabac.c:976`), Rice
   suffixes (`:952,:963`), EG prefix `while(get_cabac_bypass)` → `clz(~peek)` (`:948`),
   last-sig suffix (`:897-900`), MVD (`:803-813`). Currently one-bin-at-a-time everywhere
   (HEVC **and** VVC). Est. **2–4% of total decode**, more on high-bitrate 10/12-bit.
   *Prior art on this exact CPU:* John Cox's **rpi-ffmpeg** `get_cabac_by22_peek`/flush
   (reciprocal-based 22-bit bypass batching). The submodule README workflow anticipates
   an `rpi` remote to diff against — crib from it.
2. **CLZ renorm in the aarch64 asm** — `ff_h264_norm_shift[range] == clz32(range) − 23`
   exactly (256→0, 128→1, 2→7). Replaces a dependent 4-cycle `ldrb` with a 1-cycle `clz`;
   the C `refill2` already uses CLZ (`HAVE_FAST_CLZ`, `cabac_functions.h:96`) but the
   renorm path never got it. ~2–3 cy off a ~15-cy chain ≈ **~10–15% off `get_cabac`** for
   a ~5-line patch. Cheapest experiment available.
3. **Speculative dual-preload pipelining of the sig-map loop** (§3) — ~1–2% of total,
   medium effort, novel.
4. **Deferred NEON dequant/sign pass** (§3) — safe, zero risk. **Magnitude disputed:**
   Opus ranks this a *top* lever (much of the "10%" is this non-entropy arithmetic);
   Fable ranks it <1%. **Directly measurable — resolve empirically.**
5. **Branchy-engine variant for skewed contexts** — experiment-sized, uncertain sign.

**Not tractable:** the `greater1_ctx`/`ctx_set` recurrence (`:1329-1345`) — genuinely
serial, but tiny; any within-single-stream vectorization of the arithmetic engine; and
cross-stream SIMD CABAC (needs gather / SVE2 — A72 has neither). Stream-level parallelism
(WPP substreams / tiles / slices) is the only real cross-stream parallelism and is
already spent as **thread-level** (`hevc/hevcdec.c:2828` `hls_decode_entry_wpp`).

---

## 5. Blunt conclusion

The serial wall is **fundamental for context-coded bins**, and the reason is sharper than
"it's serial": the per-bin state maps are compact but their compositions are not (2^k
pieces), so scan and speculation both hit the same exponential blow-up, *and* control flow
makes the operator sequence data-dependent. No NEON SIMD formulation survives that; the
lane-width arithmetic kills it independently. That part of the dogma is airtight, and it's
information-theoretic, not an engineering artifact (a bin's bit-position depends on the
renorm history of all prior bins; the standard's only escape hatches are stream-granular).

But the dogma over-generalizes in two code-verifiable ways:
- **(a)** bypass bins — a large fraction of bins in *this* high-bitrate workload — form an
  affine sub-monoid whose k-fold composition collapses to one division, and the tree
  decodes them one at a time;
- **(b)** the hottest context-coded loop (significance map) lacks the context-selection
  feedback everyone cites, leaving latency-hiding headroom the current branchless-scalar
  code can't express.

Realistic combined ceiling: **~20–35% off the CABAC-related ~16%, i.e. ~3–6% of total
decode.**

---

## 6. Recommended next experiments

1. **Prototype by22-style bypass batching** in `cabac_functions.h` (crib from
   rpi-ffmpeg); rewrite `coeff_sign_flag_decode`, `coeff_abs_level_remaining_decode`,
   `last_significant_coeff_suffix_decode` to use it. This is also a **diagnostic**: it
   reveals how bypass-heavy the 16% is, which decides whether the rest of the wall is
   worth attacking. Validate bit-exact with `-f md5` (deterministic in this setup),
   measure with `perf` on `ffmpeg_g` over a long steady-state run of a 10-bit sample.
2. **Profile-split `ff_hevc_hls_residual_coding`** — isolate the dequant/level-scaling
   loop (`:1411-1434`) from the bin-parsing loop to settle the Opus↔Fable magnitude
   dispute (item 4 above). If a large share of the "10%" is that arithmetic, a NEON
   dequant kernel is a genuine low-risk win.
3. **CLZ renorm** — near-free companion patch to the engine asm (item 2).

## Key code locations
- `libavcodec/cabac_functions.h:116-181` — regular-bin engine + `get_cabac_bypass` (C).
- `libavcodec/aarch64/cabac.h` — scalar engine asm only; **no bypass asm**.
- `libavcodec/hevc/cabac.c:981-1491` — `ff_hevc_hls_residual_coding`; dequant/scaling
  `:1411-1434`; bypass call-sites `:897,:948,:952,:963,:976`; gt1/ctx_set `:1329-1345`.
- `libavcodec/hevc/hevcdec.c:2828` — WPP thread parallelism (`hls_decode_entry_wpp`).

---

## 7. Prior-art verification — rpi-ffmpeg `by22` (VERIFIED)

Follow-up to experiment #1 (§6): the `by22` bypass-batching prior art was located and
audited against the actual source. **It is real, and it ports to our aarch64 target in
pure C — no assembly required.**

**Source.** jc-kynesim/rpi-ffmpeg (already the `rpi` remote in this submodule), branches
`work/rpi_hevc_master_3` and `work/rpi_cabac_7`. Code in `libavcodec/hevc_cabac.c`
(the four `get_cabac_by22_{start,finish,peek,flush}` inlines + `bypass_start/finish`
macros + `alt1cabac_inv_range[256]` table + `hevc_mem_bits32` loader), `libavcodec/cabac.h`
(the `by22` state), and a **32-bit-only** `libavcodec/arm/hevc_cabac.h` (asm peek/flush).

**Mechanism (confirms §3's algebra exactly).** `bypass_start` repacks decoder state so
`low` holds ~22 fresh stream bits and stashes a per-`range` reciprocal; `by22_peek`
returns 22 bypass bits via one **reciprocal multiply** `((uint64_t)low*inv)>>32` — the
closed form for "R constant across a bypass run"; `flush(n,val)` consumes only the bits
actually used and refills. A whole Exp-Golomb/Rice value decodes in a few instructions
(prefix `= clz(~y)`, then suffix shifts) instead of a per-bin `while(get_cabac_bypass())`.

**Portability findings:**
1. **No aarch64 asm exists** (hand asm is 32-bit `arm/` only) — but it doesn't matter.
   `USE_BY22 = HAVE_FAST_64BIT || ARCH_ARM || ARCH_x86`; `USE_BY22_DIV = ARCH_X86`. On this
   build `HAVE_FAST_64BIT=1`, `ARCH_X86=0` (both in `config.h`), so aarch64 takes the
   **generic-C reciprocal-table path** (`alt1cabac_inv_range`, 64-bit `umulh`-style
   multiply). The batching win is available in **portable C, zero assembly**.
2. **Reimplementation, not cherry-pick.** The rpi fork is an old *flat*-layout,
   heavily-customized decoder (`s->HEVClc->cc`, `PROFILE_*`); our tree is modern nested
   `hevc/cabac.c`. The patch will not apply; the ~150-line primitive ports cleanly, the
   call-site rewrites are the labour.
3. **Target sites confirmed one-bin-at-a-time today:** `coeff_abs_level_remaining_decode`
   (`hevc/cabac.c:941`), `coeff_sign_flag_decode` (`:971`),
   `last_significant_coeff_suffix_decode` (`:892`); 29 `get_cabac_bypass` sites total.

**Scoped port:**
- Add `by22` fields to `CABACContext` in `cabac.h` (our decoder struct has no union — add
  `uint16_t bits, range`; ~free).
- Port the primitive: `alt1cabac_inv_range[256]`, `hevc_mem_bits32`, `hevc_clz32`, the four
  `get_cabac_by22_*` inlines.
- Rewrite the three decoders above to bracket the coefficient-subset loop with
  `bypass_start/finish` and use `peek/flush`.
- Bracket correctly around WPP state save/load & `cabac_reinit` (must not be mid-`by22` at
  a save point).

**Caveats:** `start/finish` has fixed overhead → only worth it for *runs* of bypass bins
(the rpi source itself notes "probably not worth it for just one value"); `CABAC_BITS=16`
matches ours; rpi-ffmpeg is LGPL (same as this tree) → adapt-with-attribution is fine.
Validate bit-exact with `-f md5`, measure with `perf` on `ffmpeg_g`.

**Verdict:** genuinely portable, no SVE/gather/asm dependency — a C primitive plus targeted
call-site rewrites, with same-CPU precedent. Low-to-medium effort; recommended prototype.
