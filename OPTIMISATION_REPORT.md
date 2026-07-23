# NTT HLS optimisation report

Notes from iterating on the Kyber NTT HLS core for the KV260 (target clock 200 MHz / 5 ns). QoR numbers below are from C-synthesis via `simulation_tester.py`.

## Where we landed

**Current best build:** out-of-place ping-pong stages, `PARALLEL=2`, group unroll `2`, plus the earlier `fqmul` and copy tweaks.

| Metric | Value |
|--------|------:|
| Max latency | **5382 cycles** (about 26.9 µs) |
| LUT | **67%** |
| DSP | 78 |
| FF | 33% |
| Timing | Met (about 3.65 ns estimated) |
| Switch | `NTT_USE_PINGPONG=1` in `hls/src/ntt_top.cpp` |

Set `#define NTT_USE_PINGPONG 0` to restore the older in-place path (about 27.6k cycles, about 27% LUT).

## Progress at a glance

| Step | Max latency | Time @ 200 MHz | LUT | Notes |
|------|------------:|---------------:|----:|-------|
| In-place `PARALLEL=4` | 29348 | 0.147 ms | 28% | Solid baseline after partition / II fixes |
| + `fqmul` / Montgomery | 28004 | 0.140 ms | 28% | Inline + DSP binds |
| + `COPY_PARALLEL=8` | 27556 | 0.138 ms | 27% | Wider load/store copies |
| + ping-pong (2×2) | **5382** | **26.9 µs** | **67%** | **Best so far** |
| Ping-pong (4×2) | 7654 | 38.3 µs | 85% | Reverted (slower, hungrier) |
| Early aggressive (4×4) | 5191 | 26 µs | **121%** | Too big; timing failed |

---

## 1. Tighten `fqmul` / Montgomery

### Context

Bigger architecture experiments (more group parallelism, out-of-place stages) either blew the LUT budget or made C-synthesis crawl. We stuck with in-place NTT, four butterflies per cycle, fully partitioned local coeffs, and roughly one-minute synth times.

From that baseline (about 29.3k cycles, about 28% LUT), the useful next step was the multiply-and-reduce path every butterfly hits, not more parallelism.

### Changes

`fqmul` is “multiply two coeffs, then Montgomery-reduce mod q”. It used to be a thin wrapper around reference `montgomery_reduce`. For HLS we:

1. Inlined `fqmul` and `montgomery_reduce` so the butterfly pipeline sees the whole path.
2. Spelled out the Montgomery steps more clearly (low half of the product, × QINV, × q, shift), without changing Kyber maths.
3. Bound those multiplies to DSPs (`BIND_OP` / `impl=dsp`).
4. Applied the same idea to `barrett_reduce` on the inverse NTT path, mainly for consistency.

Loop structure, twiddle table, and testbench check (`invntt(ntt(r)) == r * MONT mod q`) were unchanged.

### Result

- Max latency: 29348 → 28004 cycles (about 4.6%).
- Absolute time: 0.147 ms → 0.140 ms.
- Area stayed flat (about 28% LUT, 12 DSPs); timing still met 5 ns.

Modest but cheap: same footprint, shorter arithmetic inside each butterfly, still synthesises quickly and fits.

---

## 2. Widen the copy in / copy out

### Context

After `fqmul`, the next cheap target was bookkeeping: copy 256 coeffs into a local array, run the NTT, copy them back. At one coeff per cycle that was about 512 cycles of overhead on top of the transform.

### Changes

Butterfly schedule unchanged (still in-place `PARALLEL=4`). Only the two copy loops:

1. Step by `COPY_PARALLEL = 8` instead of 1.
2. Unroll the inner lane so eight loads or stores can issue in one pipeline beat (`PIPELINE II=1` on the outer loop).

### Result

- Max latency: 28004 → **27556** cycles (448 cycles, about 1.6%).
- Absolute time: 0.140 ms → 0.138 ms.
- Resources flat or slightly better (about 27% LUT, 12 DSPs).

Matches the expected saving (256+256 → 32+32 beats). Low risk, no rewrite of the NTT itself.

---

## 3. Out-of-place ping-pong (current best)

### Context

An earlier aggressive stage rewrite (`PARALLEL=4`, group unroll 4) hit about 5.2k cycles but about 121% LUT and failed timing. Sharing one `ntt_stage` with `INLINE off` then hung C-synth for a long time. We brought the idea back with quieter knobs.

### Changes

Behind `NTT_USE_PINGPONG`:

1. Two fully partitioned buffers (`buf_a` / `buf_b`). Each stage reads one and writes the other (no in-place hazards).
2. Seven explicit stages (`len = 128 … 2`) so HLS sees fixed geometry. After an odd number of swaps the result sits in `buf_b`.
3. Knobs vs the over-budget build: **`PARALLEL=2`**, **group unroll factor=2** (was 4 and 4).
4. Kept `fqmul` / Montgomery DSP work and `COPY_PARALLEL=8`.

### Result

| Metric | In-place (after copies) | Ping-pong 2×2 |
|--------|------------------------:|--------------:|
| Max latency | 27556 cycles | **5382 cycles** |
| Time @ 200 MHz | 0.138 ms | **26.9 µs** |
| LUT | 27% | **67%** |
| DSP | 12 | 78 |
| FF | 4% | 33% |
| Timing | OK | OK |

About a **5×** latency cut, and it still fits the KV260 with headroom. Not quite as wild as the old 5.2k / 121% attempt, but this one is actually placeable.

### What we tried next (and reverted)

Bumping to `PARALLEL=4` with group unroll still 2:

- Synth fine (about 77 s), still fitted (about 85% LUT).
- Max latency went the wrong way: **5382 → 7654** cycles (about 27 µs → about 38 µs).
- DSPs doubled to 156.

More inner parallelism made a heavier schedule, not a faster one. Reverted to **PARALLEL=2**, group unroll **2**.

---

## Takeaways

1. Clean arithmetic (`fqmul` / Montgomery) and wider copies were worth doing early; cheap latency for almost no area.
2. The big win was out-of-place staging with **modest** group parallelism, not maxing every unroll factor.
3. Past a point, wider butterflies can hurt: deeper pipelines and more LUTs with worse (or flat) latency.
4. Keep the 2×2 ping-pong build as default until place-and-route on the board says otherwise.
