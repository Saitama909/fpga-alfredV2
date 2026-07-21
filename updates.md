
# Updates

## Changelog
21/07/2026 - HLS NTT optimisation + simulation tester tooling.

**`hls/src/ntt_top.cpp` (`ntt`)**
- Kept the Kyber reference Cooley–Tukey NTT math; tuned it for Vitis HLS.
- Copy coeffs into a local `local_r[256]` and `#pragma HLS ARRAY_PARTITION … complete` so each element is a register (needed for parallel butterfly access; fixed the earlier II=3 resource-limit issue).
- Issue **multiple butterflies per cycle** via `PARALLEL` (best so far: **4**): the inner loop steps by `PARALLEL` and fully unrolls that many lanes (`fqmul` + add/sub) under `#pragma HLS PIPELINE II=1`.
- `PARALLEL = 8` was tried and **regressed** latency (~29k → ~42k cycles) while using more DSP/LUT — deeper/heavier pipeline beat the fewer trip counts. Prefer `PARALLEL = 4` for now.
- Rough latency trend @ 200 MHz (max cycles): baseline partitioned ~62.5k → parallel-4 **~29.3k**.

**`simulation_tester.py` + `SIMULATION_TESTER.md`**
- Headless Unified Vitis flow: C-sim → C-synth → parse QoR reports.
- Flags: `--console-print`, `--no-save`, `--dry-run`.
- Saves readable markdown under `results/results-<unix>.md` (headline metrics + proper markdown tables).

03/07/2026 4:30 - Added initial project plan presentation PDF as per the project deliverables requirements for week 5.
