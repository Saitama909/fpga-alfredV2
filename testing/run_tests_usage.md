# Test runner

## What is this?

`run_tests.py` is the HLS test harness. It reads `testing/config.txt`, then runs whichever steps you've turned on. Each step lives in its own file under `testing/tests/`.

Typical flow is C-sim -> C-synth -> RTL co-sim -> a compare of the two. There's also a hardware-build hook, but that's just a stub for now.

You need a Unified Vitis workspace / component (the paths in the config point at that, not this repo's `hls/` folder).

## How to run

From the repo root:

```bash
python3 testing/run_tests.py
```

That wipes `testing/logs` and `testing/outputs` first (unless you turn `PURGE_FOLDERS` off), then runs the enabled steps and prints a summary at the end.

### `--purge`

Clears `testing/logs` and `testing/outputs` (keeps `.gitkeep`), then exits. Handy if you just want the folders emptied without kicking off a run. A normal run already purges those folders first.

```bash
python3 testing/run_tests.py --purge
```

That's the only flag. Everything else is in `config.txt`.

---

## `config.txt`

Comment a line with `#`, or set a `RUN_*` flag to `0` / `false` / `no` / `off` to skip that step. `1` / `true` / `yes` / `on` all count as enabled.

### Paths

| key              | what it is                                                                  |
| ---------------- | --------------------------------------------------------------------------- |
| `WORKSPACE_PATH` | Unified Vitis workspace (must exist)                                        |
| `COMPONENT_NAME` | component under that workspace (e.g. `ntt_core`)                            |
| `VITIS_BIN_DIR`  | folder with `vitis-run` and `v++`. Leave empty if they're already on `PATH` |

### Output

| key                      | what it does                                                            |
| ------------------------ | ----------------------------------------------------------------------- |
| `SUPPRESS_INFO_MESSAGES` | hide lines that start with `INFO:` (WARNING / ERROR / PASS still show)  |
| `PURGE_FOLDERS`          | clear `testing/logs` and `testing/outputs` before each run (default on) |

### Steps

| key            | what it runs                                                                         |
| -------------- | ------------------------------------------------------------------------------------ |
| `RUN_CSIM`     | C simulation (`vitis-run --csim`)                                                    |
| `RUN_CSYNTH`   | C synthesis: latency, II, resource utilisation                                       |
| `RUN_COSIM`    | RTL co-sim: checks the generated RTL against the C testbench                         |
| `RUN_COMPARE`  | side-by-side of csim vs cosim (pass/fail + latency). Needs both of those to have run |
| `RUN_HW_BUILD` | stub only. Always comes out as SKIPPED and never fails the run                       |

### Utilisation bands

These are percentages from C-synth (BRAM / DSP / FF / LUT / URAM):

| key         | default | meaning                            |
| ----------- | ------- | ---------------------------------- |
| `UTIL_WARN` | 70      | yellow warning                     |
| `UTIL_HIGH` | 90      | still a warning, just louder       |
| `UTIL_OVER` | 100     | treated as an error in the summary |

They don't fail the run on their own, they just show up under "Resource warnings".

---

## What you get

Logs go in `testing/logs/` (`csim-log.txt`, `csynth-log.txt`, …). Each one gets a `===== Test Result ======` footer with PASSED/FAILED plus warning/error counts.

Summaries go in `testing/outputs/`:

- `run-summary.txt`, the pass/fail table, timing, module intervals, usage
- `csynth-summary.txt` / `cosim-summary.txt` / `compare-summary.txt`, per-step bits
- copied reports (e.g. `cosim-report.rpt`) when Vitis writes them

The branch tester reads `run-summary.txt` (and the cosim/csynth summaries) when you `--compare` branches.

---

## Notes

- If `vitis-run` / `v++` aren't on `PATH`, set `VITIS_BIN_DIR` or you'll get a "Can't find …" error.
- `WORKSPACE_PATH` has to be a real directory, and the component needs `hls_config.cfg`.
- Cosim is the slow one. Turn it off in the config if you only want a quick csim/csynth pass.
- Compare will fail if csim or cosim was skipped or failed — that's deliberate.
