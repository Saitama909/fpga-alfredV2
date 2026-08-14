# fpga-alfredV2

## Project Overview: Number Theoretic Transform (NTT) Core Acceleration for PQC

<div align="left">
 
[![Platform](https://img.shields.io/badge/Platform-Kria%20KV260-e8710a?logo=amd&logoColor=white)](https://www.amd.com/en/products/system-on-modules/kria/k26/kv260-vision-starter-kit.html)
[![Toolchain](https://img.shields.io/badge/Vitis%20HLS-2025.2-1a91da)](https://www.amd.com/en/products/software/adaptive-socs-and-fpgas/vitis.html)
[![Scheme](https://img.shields.io/badge/Scheme-Kyber--768%20(ML--KEM)-2ea44f)](https://pq-crystals.org/kyber/)
 
</div>

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

- **Host Platform:** x86 based CPU running Ubuntu 24.04.
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

> If you're unsure of the exact namespaced function name, run just the preprocessor - `gcc -E hls/src/ntt_top.cpp` - and look for `pqcrystals_kyber768_ref_ntt`.
