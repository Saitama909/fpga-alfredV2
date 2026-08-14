# fpga-alfredV2

## Project Overview: Number Theoretic Transform (NTT) Core Acceleration for PQC

<div align="left">
[Platform](https://www.amd.com/en/products/system-on-modules/kria/k26/kv260-vision-starter-kit.html)
[Toolchain](https://www.amd.com/en/products/software/adaptive-socs-and-fpgas/vitis.html)
[Scheme](https://pq-crystals.org/kyber/)
<div>

See [COMP4601 Initial Project Plan](https://github.com/Saitama909/fpga-alfredV2/blob/main/COMP4601%20Initial%20Project%20Plan-1.pdf)

## Repository

The development and source code for this project are maintained here: [https://github.com/Saitama909/fpga-alfredV2](https://github.com/Saitama909/fpga-alfredV2)

## Team Members

- **Riley Haydon** (z5416346)
- **Jovan Xin** (z5491897)
- **William Chan** (z5481729)
- **Aditya Muthukattu** (z5422156) — _Team Representative_

## Contact Information

For any inquiries, project updates, or further information, please contact our team representative:

- **Contact:** Aditya Muthukattu
- **Email:** [a.muthukattu@student.unsw.edu.au](mailto:a.muthukattu@student.unsw.edu.au)

## Getting Started

### Prequisites

> The following was the test and development environment that worked during the course of the project.

- **Dev Platform:** x86 based CPU running Ubuntu 24.04.
- **Software:** Vitis / Vitis HLS 2025.2 (ships the aarch64 cross-toolchain so nothing extra to install).
- **Deployment Platform:** Kria KV260 running PetaLinux version 2025.1 with XRT.
- **Reference Benchmarking:** The pq-crystals reference for benchmarking: `git clone https://github.com/pq-crystals/kyber`.

### Create the Vitis Workspace and Verify it Runs

1. Clone this repo.
2. Create a new Vitis workspace with an **HLS component** named `ntt_core`:

- Add all of `hls/src/` and the testbenches from `hls/testbench/`
    - **Hardware tab** -> Platform -> `kv260_custom` (or the generic Xilinx KV260 platform if you aren't deploying yet)
    - **Settings tab** -> clock target `200MHz`, flow target `vitis`, `package.output.format` = `xo`

3. Set the top-level function in `hls_config.cfg`: under `hls.syn.top`, enter the namespaced name - by default `pqcrystals_kyber768_ref_ntt` (or `..._poly_mul_batch` for the full batched kernel).

4. **Run C simulation** - verify all testbenches pass.
5. **Run synthesis** (then cosim / export as needed).

> If you're unsure of a namespaced poly-mul name, run the preprocessor — `gcc -E hls-kyber/src/ntt_top.cpp` — and look for `pqcrystals_kyber768_ref_poly_mul_batch` / `..._ntt`.

## Switching kernels

We ended up with several HLS trees that share a lot of the same files (`ntt_top.cpp`, reduction, params, …):

- **`hls-kyber/`** — Kyber IND-CPA encrypt (`kyber_enc_core`). This is what's in the repo and what the host in `ntt_host/` talks to.
- **The poly-mul / NTT work** — batched `poly_mul` / `poly_mul_batch` (and the standalone NTT). That's zipped up separately in `polymul-vitis-workspace-archive.zip` for the submission rather than living next to `hls-kyber/` in this tree.

Ideally `ntt_top` and the shared bits get merged into one folder with two tops. Due to time restrictions, they're kept apart instead of risking a last-minute merge that breaks both.

### How to switch what Vitis is building

`testing/run_tests.py` just talks to whatever HLS component you pointed it at in `testing/config.txt` (`WORKSPACE_PATH` + `COMPONENT_NAME`). It doesn't care which sources that component is using. You switch by changing the component, but not the runner!

1. In the Vitis HLS component (`ntt_core` by default), drop the current `src/` + `testbench/` files and add one of:
    - `hls-kyber/src` + `hls-kyber/testbench` (Kybe r encrypt), **or**
    - the unzipped poly-mul tree's `src` + `testbench`
2. Set `hls.syn.top` in `hls_config.cfg`:
    - Kyber encrypt: ` kyber_enc_core`
    - Batched poly-mul: `pqcrystals_kyber768_r ef_poly_mul_batch`
    - NTT only: `pqcrystals_kyber768_ref_ntt`
3. Re-run C-sim / synth / cosim as usual, or `python3 testing/run_tests.py` from the repo root.

The host in `ntt_host/` is wired for `kyber_enc_core`. If you switch the HLS top to poly-mul, don't expect that host to still match.
