# fpga-alfredV2

## Project Overview: Number Theoretic Transform (NTT) Core Acceleration for PQC

We present a system that accelerates the Number-Theoretic Transform at the heart of ML-KEM (Kyber-768) post-quantum key exchange on a Xilinx KV260, targeting the polynomial multiplication that dominates the scheme's runtime. A Vitis HLS kernel implements the full forward NTT, pointwise multiply and inverse NTT as a staged DATAFLOW pipeline at 200 MHz, batching up to 256 independent polynomial products into a single dispatch to amortise host–device overhead. Measured on hardware, the accelerator sustains roughly 4.15 million multiplies per second - 0.24 µs per product against 27.8 µs for the pq-crystals reference on the board's Cortex-A53 (@ max freq 1.33GHz, `-O3` optimised), a ~115x speedup. We also provided demonstrations of speedup of our core integrated into the encryption stage of Kyber, translating to a 6.81x kernel speedup, proving applicability and correctness.

<div align="left">

[![Platform](https://img.shields.io/badge/Platform-Kria%20KV260-e8710a?logo=amd&logoColor=white)](https://www.amd.com/en/products/system-on-modules/kria/k26/kv260-vision-starter-kit.html)
[![Toolchain](https://img.shields.io/badge/Vitis%20HLS-2025.2-1a91da)](https://www.amd.com/en/products/software/adaptive-socs-and-fpgas/vitis.html)
[![Scheme](<https://img.shields.io/badge/Scheme-Kyber--768%20(ML--KEM)-2ea44f>)](https://pq-crystals.org/kyber/)

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

6. Set up the host and system components exactly as we did in the labs earlier in the course.
7. Unzip and refer to polymul-workspace-archive.zip (in the repo) for all the configurations to build/link/generate bitstream. Make sure compiler settings are right (eg. set C++ version correctly and optimisation level to -O3 so that this task is actually a challenge. Pay attention to `nttWS/ntt_sys/hw_link/binary_container_1-link.cfg` that sets up m_axi ports and connectivity.
8. Refer to this link here for the archive of the workspace containing build outputs for polymul: [Google Drive](https://drive.google.com/file/d/1N7v2GQ6YgC-X4rv6CqLQcMcOd18iNnI6/view?usp=drive_link)


### Switching Kernels/Host files

To switch between our different demonstrations (forward NTT speedup, batched polymul (project goal, headline number), and integration into kyber encryption.
Just substitute in the source files from different `hls-*` folders and corresponding host folders.


### Deployment

Copy the artifacts over:

```sh
ssh petalinux@10.42.0.168 mkdir -p ~/ntt_deploy
scp nttWS/ntt_sys/build/hw/hw_link/binary_container_1.xclbin \
    nttWS/ntt_host/build/hw/ntt_host \
    petalinux@10.42.0.168:/home/petalinux/ntt_deploy/
```

The PL must be programmed before the host runs. On the KV260 that means a
firmware package in `/lib/firmware/xilinx/<name>/` three files, where the
`.bin` **must be named after its directory**:

```sh
# on the board, first time only
sudo mkdir -p /lib/firmware/xilinx/ntt_host
sudo cp /lib/firmware/xilinx/dft/pl.dtbo    /lib/firmware/xilinx/ntt_host/
sudo cp /lib/firmware/xilinx/dft/shell.json /lib/firmware/xilinx/ntt_host/
```

Deploy new bitstream:
```sh
sudo cp ~/ntt_deploy/binary_container_1.xclbin /lib/firmware/xilinx/ntt_host/ntt_host.bin
sudo xmutil unloadapp
sudo xmutil loadapp ntt_host
md5sum /lib/firmware/xilinx/ntt_host/ntt_host.bin   # confirm it matches your build
```

### Run and results:
```sh
cd ~/ntt_deploy
chmod +x ntt_host
taskset -c 0 ./ntt_host -x ./binary_container_1.xclbin -d 0 -n 500 -b 256 -q 2
```

| flag | meaning | default |
|---|---|---|
| `-x` | xclbin path | — |
| `-d` | device index | `0` |
| `-n` | timed dispatches per series | `1000` |
| `-b` | polynomials per batch dispatch (max 256) | `1` |
| `-q` | in-flight ring slots | `2` |

Expected output
``` txt
Open the device 0
Load the xclbin ./binary_container_1_b3.xclbin
Open the kernel pqcrystals_kyber768_ref_poly_mul_batch
Allocate Buffer in Global Memory
synchronize input buffer data to device global memory
Execution of the kernel
Get the output data from the device
*******************************************
PASS: all 256 slot(s) match the schoolbook product
*******************************************
software baseline verified against schoolbook product

  dispatch decomposition (k runs enqueued, one wait):
    k= 1  total    91.47 us   per dispatch   91.47 us
    k= 2  total   153.18 us   per dispatch   76.59 us
    k= 4  total   277.53 us   per dispatch   69.38 us
    k= 8  total   522.97 us   per dispatch   65.37 us
    k=16  total  1019.70 us   per dispatch   63.73 us
    fit: device   61.86 us/dispatch, fixed overhead   29.44 us
         device per multiply:  0.242 us
independent ring verified: 2 slots, 256 polynomials each

===== poly_mul: 500 trials, batch 256, queue depth 2 =====
  all times are PER POLYNOMIAL MULTIPLY
  hardware: 128000 multiplies (500 dispatches x 256 per dispatch)
  software: 500 multiplies (1 per trial)
                            median      mean       min       max    stddev
                              (us)      (us)      (us)      (us)      (us)
  hardware (kernel)           0.35      0.35      0.34      0.42      0.01
  hardware (ring resident)      0.24      0.24      0.10      0.36      0.01
  hardware (ring e2e)         0.27      0.26      0.14      0.46      0.11
  hardware (serial e2e)       0.47      0.47      0.45      0.67      0.01
  software (A53)             27.80     27.86     27.73     39.43      0.75

  speedup, kernel only  (median):  78.96x
  speedup, ring resident (median): 115.35x   (depth 2)
  speedup, ring e2e      (median): 102.53x
  ring gain over serial kernel:      1.46x
  speedup, serial e2e    (median):  59.66x
  serial sync overhead per multiply:   0.11 us
  dispatch cost amortised over:      256 multiplies
  throughput, serial e2e:         2146689 multiplies/s
  throughput, ring resident:      4154725 multiplies/s
  throughput, ring e2e:           3862502 multiplies/s
  mean response, ring e2e batch:   66.28 us
  note: queued completion medians are bursty; use mean throughput
====================================================================
```
