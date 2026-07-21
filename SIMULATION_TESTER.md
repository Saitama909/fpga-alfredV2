# Simulation Tester



`simulation_tester.py` drives a **Unified Vitis** HLS component headlessly:

1. Run **C simulation** (`vitis-run --csim`)
2. If that passes, run **C synthesis** (`v++ -c --mode hls`)
3. Parse the synthesis QoR reports and print / save the useful tables

It is intended for the existing `ntt_core` HLS component whose sources live in this repo (`hls/src`, `hls/testbench`).

---

## Contents

* [1. Requirements](#requirements)
* [2. Basic usage](#basic-usage)
* [3. Command-line options](#command-line-options)
* [4. Results output](#results-output)
* [5. Exit codes](#exit-codes)
* [6. Troubleshooting](#troubleshooting)

---

## Requirements

### 1. Unified Vitis installed

This script expects AMD **Vitis Unified** (tested with **2025.2**) and the CLI tools:

- `vitis-run`
- `v++`

### 2. Set `VITIS_BIN_DIR`

Open `simulation_tester.py` and set the install `bin` directory at the top of the file:

```python
VITIS_BIN_DIR = "/home/riley/Xilinx/2025.2/Vitis/bin"
```

If you leave this empty (`""`), the script will try `PATH` via `shutil.which`. Setting it explicitly is recommended.

### 3. Set `WORKSPACE_PATH` (and component name)

Point the script at your **already-created** Unified Vitis workspace that contains the HLS component:

```python
WORKSPACE_PATH = "/home/riley/Desktop/COMP4601/project-repo"
COMPONENT_NAME = "ntt_core"
```

Expected layout:

```text
<WORKSPACE_PATH>/
  ntt_core/                 # COMPONENT_NAME
    hls_config.cfg          # must reference this repo's hls/src + testbench
    vitis-comp.json         # used to discover work_dir (e.g. fqmul)
    fqmul/                  # HLS work directory (name may differ)
```

The component's `hls_config.cfg` should already point at this repository's sources, for example:

- `hls/src/ntt_top.cpp` (top: `pqcrystals_kyber768_ref_ntt`)
- other `hls/src/*` files
- `hls/testbench/ntt_tb.cpp`

See `PROJECT_SETUP.md` for creating that Vitis HLS component the first time.

### 4. Python

Python 3.10+ recommended (uses `list[str] | None` style typing). No third-party packages required.

---

## Basic usage

From the repo root:

```bash
python3 simulation_tester.py
```

Default behaviour:

| Step | What happens |
|------|----------------|
| C-sim | Runs; must PASS before synthesis |
| C-synth | Runs after a passing sim |
| Console | Prints **Timing summary** + **Latency summary** only |
| Disk | Writes full tables to `results/results-<UNIX_TIMESTAMP>.txt` |

While tools are running, the console updates a single line:

```text
working on it 12
```

---

## Command-line options

### `--console-print`

Print the **full** QoR tables to the console, in addition to the default summaries:

- full Timing section
- full Latency section (including instance/loop detail)
- Performance & Resource Estimates (modules/loops, II issues, BRAM/DSP/FF/LUT)

```bash
python3 simulation_tester.py --console-print
```

Without this flag, the console stays short (timing + latency summaries only). Full tables are still saved to `results/` unless saving is disabled.

### `--no-save`

Do **not** write a file under `results/`.

Console behaviour is unchanged (summaries by default; full tables if `--console-print` is also set).

```bash
python3 simulation_tester.py --no-save
python3 simulation_tester.py --no-save --console-print
```

### `--dry-run`

Run **only** C simulation and C synthesis to verify they succeed.

- Does **not** parse synthesis reports
- Does **not** save anything under `results/`
- Ignores the usual QoR console dump (success/failure messages still print)

```bash
python3 simulation_tester.py --dry-run
```

Useful as a quick smoke test after changing HLS source or Vitis config.

### Combining flags

| Command | Console | Save file |
|---------|---------|-----------|
| `python3 simulation_tester.py` | summaries | yes (`results/results-<ts>.txt`) |
| `python3 simulation_tester.py --console-print` | summaries + full tables | yes |
| `python3 simulation_tester.py --no-save` | summaries | no |
| `python3 simulation_tester.py --console-print --no-save` | summaries + full tables | no |
| `python3 simulation_tester.py --dry-run` | sim/synth status only | no |

`--dry-run` always skips saving (same end result as `--no-save` for files, but also skips report parsing).

---

## Results output

Unless `--no-save` or `--dry-run` is set, the script creates:

```text
results/results-<UNIX_TIMESTAMP>.txt
```

Example: `results/results-1753080000.txt`

The file contains:

1. A short header (timestamp + report paths)
2. Timing Estimates
3. Latency Estimates (cycles, including detail)
4. Performance & Resource Estimates

The `results/` folder is created automatically next to `simulation_tester.py` (repo root).

---

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success (and dry-run success) |
| `1` | Missing workspace/tools, C-sim fail, C-synth fail, or missing reports |

---

## Troubleshooting

**`Could not find 'vitis-run'` / `v++`**  
Update `VITIS_BIN_DIR`, or ensure those tools are on your `PATH`.

**`WORKSPACE_PATH does not exist` / `Component directory not found`**  
Fix `WORKSPACE_PATH` and `COMPONENT_NAME` so they match your Unified Vitis workspace.

**C Simulation failed**  
Check the testbench and HLS sources. The script prints the last lines of tool output on failure. Also inspect:

```text
<work_dir>/hls/csim/report/*_csim.log
```

**C Synthesis finished but report missing**  
Confirm synthesis wrote:

```text
<work_dir>/hls/syn/report/*_csynth.rpt
<work_dir>/hls/syn/report/csynth.rpt
```

**Resource / II issues**  
Look at the Performance & Resource table (saved file, or `--console-print`) for `Issue Type` / `Violation Type` on the inner NTT loops.
