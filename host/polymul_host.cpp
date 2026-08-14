/*
 * XRT host application for the poly_mul kernel.
 *
 * Loads the xclbin, runs batched polynomial multiplication in
 * Rq = Zq[X]/(X^256+1) on the PL, checks every result against an independent
 * schoolbook convolution, then profiles the accelerator against the
 * pq-crystals reference NTT running on the A53 with the same timing code.
 *
 * Four hardware series are reported, all normalized per polynomial:
 *   kernel         -- one synchronous dispatch, buffers already resident
 *   ring resident  -- independent BOs kept in flight, without timed syncs
 *   ring e2e       -- independent BOs with sync and dispatch overlapped
 *   serial e2e     -- synchronous input syncs, dispatch, and output sync
 *
 * Mean ring throughput is the primary batching metric. Per-completion ring
 * medians are bursty because one wait can reap work completed in parallel.
 */

#include "cmdlineparser.h"
#include "sw_poly_mul.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

// XRT includes
#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

/* Name is mangled by KYBER_NAMESPACE in hls/src/params.h; it must match the
   kernel name in the .xo (see ntt/hls/kernel.xml in the Vitis workspace). */
#define KERNEL_NAME       "pqcrystals_kyber768_ref_poly_mul"
#define KERNEL_BATCH_NAME "pqcrystals_kyber768_ref_poly_mul_batch"

static constexpr int N = SW_KYBER_N;
static constexpr size_t POLY_BYTES = N * sizeof(int16_t);

/* Host-side validation contract. MAX_BATCH must match POLY_MUL_MAX_BATCH in
   hls/src/ntt_top.h for the xclbin being deployed. The runtime kernel argument
   selects any count up to that synthesized maximum. */
static constexpr int MAX_BATCH = 256;
static constexpr int MAX_QUEUE_DEPTH = 64;

static void bind_run_arguments(xrt::run& run, const xrt::bo& bo_a,
                               const xrt::bo& bo_b, const xrt::bo& bo_r,
                               unsigned batch, bool using_batch_kernel) {
    run.set_arg(0, bo_a);
    run.set_arg(1, bo_b);
    run.set_arg(2, bo_r);
    if (using_batch_kernel) run.set_arg(3, batch);
}

struct RingSlot {
    xrt::bo bo_a;
    xrt::bo bo_b;
    xrt::bo bo_r;
    int16_t* h_a;
    int16_t* h_b;
    int16_t* h_r;
    xrt::run run;

    RingSlot(xrt::device& device, xrt::kernel& kernel, size_t bytes,
             unsigned batch, bool using_batch_kernel)
        : bo_a(device, bytes, kernel.group_id(0)),
          bo_b(device, bytes, kernel.group_id(1)),
          bo_r(device, bytes, kernel.group_id(2)),
          h_a(bo_a.map<int16_t*>()),
          h_b(bo_b.map<int16_t*>()),
          h_r(bo_r.map<int16_t*>()),
          run(kernel) {
        bind_run_arguments(run, bo_a, bo_b, bo_r, batch, using_batch_kernel);
    }
};

/* Deterministic, slot-specific stimulus. Distinct slots must produce distinct
   results so a run using the wrong BO addresses cannot pass verification. */
static void initialize_ring_slot(RingSlot& slot, int n_coeff, unsigned seed) {
    uint32_t state = seed;
    for (int i = 0; i < n_coeff; ++i) {
        state = state * 1664525u + 1013904223u;
        slot.h_a[i] = sw_center_mod_q(state % SW_KYBER_Q);
        state = state * 1664525u + 1013904223u;
        slot.h_b[i] = sw_center_mod_q(state % SW_KYBER_Q);
        slot.h_r[i] = 0x7fff;
    }
}

static int verify_ring_slot(const RingSlot& slot, int batch, int slot_index) {
    int16_t expected[N];
    int errors = 0;
    for (int n = 0; n < batch; ++n) {
        sw_poly_mul_schoolbook(&slot.h_a[n * N], &slot.h_b[n * N], expected);
        for (int i = 0; i < N; ++i) {
            const int index = n * N + i;
            if (sw_center_mod_q(slot.h_r[index]) != expected[i]) {
                if (errors < 3) {
                    printf("RING MISMATCH queue slot %d batch slot %d coeff %d: "
                           "got %d, expected %d\n",
                           slot_index, n, i, sw_center_mod_q(slot.h_r[index]),
                           expected[i]);
                }
                ++errors;
            }
        }
    }
    return errors;
}

struct Timings {
    double min_us, max_us, mean_us, median_us, stddev_us;
};

static Timings compute_stats(std::vector<double> samples_us) {
    Timings s{};
    std::sort(samples_us.begin(), samples_us.end());
    size_t n = samples_us.size();
    s.min_us = samples_us.front();
    s.max_us = samples_us.back();
    double sum = std::accumulate(samples_us.begin(), samples_us.end(), 0.0);
    s.mean_us = sum / n;
    s.median_us = (n % 2 == 0) ? (samples_us[n / 2 - 1] + samples_us[n / 2]) / 2.0
                               : samples_us[n / 2];
    double sq_sum = 0.0;
    for (double v : samples_us) sq_sum += (v - s.mean_us) * (v - s.mean_us);
    s.stddev_us = std::sqrt(sq_sum / n);
    return s;
}

/* One timed series: `warmup` untimed iterations, then `trials` timed ones,
   each normalised by `divisor`. 
   
   The body takes the  iteration index so the ring series can pick its slot. */
template <typename F>
static Timings time_series(int warmup, int trials, double divisor, F&& body) {
    for (int t = 0; t < warmup; ++t) body(t);

    std::vector<double> us;
    us.reserve(trials);
    for (int t = 0; t < trials; ++t) {
        auto t0 = std::chrono::steady_clock::now();
        body(t);
        auto t1 = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / divisor);
    }
    return compute_stats(us);
}

static void print_row(const char* label, const Timings& s) {
    printf("  %-22s %9.2f %9.2f %9.2f %9.2f %9.2f\n", label, s.median_us, s.mean_us,
           s.min_us, s.max_us, s.stddev_us);
}

int main(int argc, char** argv) {
    // Command Line Parser
    sda::utils::CmdLineParser parser;

    // Switches
    //**************//"<Full Arg>",   "<Short Arg>", "<Description>",            "<Default>"
    parser.addSwitch("--xclbin_file", "-x", "input binary file string", "");
    parser.addSwitch("--device_id", "-d", "device index", "0");
    parser.addSwitch("--iterations", "-n", "timed dispatches per series", "1000");
    parser.addSwitch("--batch", "-b", "polynomials per batch dispatch", "1");
    parser.addSwitch("--queue-depth", "-q", "independent in-flight ring slots", "2");
    parser.parse(argc, argv);

    // Read settings
    std::string binaryFile = parser.value("xclbin_file");
    int device_index = stoi(parser.value("device_id"));
    int num_trials = stoi(parser.value("iterations"));
    int batch = stoi(parser.value("batch"));
    int queue_depth = stoi(parser.value("queue-depth"));

    /* Unbuffered: if the board wedges, whatever was printed up to that point has
       already left. Buffered output would be lost with the machine. */
    std::cout << std::unitbuf;

    /* defensive programming :) */
    if (binaryFile.empty()) {
        std::cerr << "error: no xclbin given (-x <file>)\n\n";
        parser.printHelp();
        return EXIT_FAILURE;
    }
    if (!sda::utils::is_file(binaryFile)) {
        std::cerr << "error: xclbin not found: " << binaryFile << "\n";
        return EXIT_FAILURE;
    }
    if (num_trials < 1) {
        std::cerr << "error: --iterations must be >= 1\n";
        return EXIT_FAILURE;
    }
    if (batch < 1 || batch > MAX_BATCH) {
        std::cerr << "error: --batch must be between 1 and " << MAX_BATCH << "\n";
        return EXIT_FAILURE;
    }
    if (queue_depth < 1 || queue_depth > MAX_QUEUE_DEPTH) {
        std::cerr << "error: --queue-depth must be between 1 and "
                  << MAX_QUEUE_DEPTH << "\n";
        return EXIT_FAILURE;
    }

    const int    n_coeff = batch * N;
    const size_t buf_bytes = n_coeff * sizeof(int16_t);

    xrt::device device;
    xrt::kernel krnl;
    bool using_batch_kernel = false;
    try {
        std::cout << "Open the device " << device_index << std::endl;
        device = xrt::device(device_index);
        std::cout << "Load the xclbin " << binaryFile << std::endl;
        auto uuid = device.load_xclbin(binaryFile);
        /* An xclbin holds one or the other. Try the runtime-count batch kernel
           first, then retain compatibility with the single-polynomial top. */
        try {
            std::cout << "Open the kernel " << KERNEL_BATCH_NAME << std::endl;
            krnl = xrt::kernel(device, uuid, KERNEL_BATCH_NAME);
            using_batch_kernel = true;
        } catch (const std::exception&) {
            std::cout << "  not present; falling back to " << KERNEL_NAME << std::endl;
            krnl = xrt::kernel(device, uuid, KERNEL_NAME);
            if (batch != 1) {
                std::cerr << "error: xclbin holds the single-polynomial kernel, "
                             "so --batch must be 1\n";
                return EXIT_FAILURE;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "XRT error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Allocate Buffer in Global Memory\n";
    auto bo_a = xrt::bo(device, buf_bytes, krnl.group_id(0));
    auto bo_b = xrt::bo(device, buf_bytes, krnl.group_id(1));
    auto bo_r = xrt::bo(device, buf_bytes, krnl.group_id(2));

    // pointers to host memory backing the device buffers
    auto h_a = bo_a.map<int16_t*>();
    auto h_b = bo_b.map<int16_t*>();
    auto h_r = bo_r.map<int16_t*>();

    // Same stimulus as hls/testbench/poly_mul_tb.cpp, so a hardware mismatch
    // can be reproduced in csim/cosim directly.
    srand(42);
    for (int i = 0; i < n_coeff; i++) {
        h_a[i] = sw_center_mod_q(rand() % SW_KYBER_Q);
        h_b[i] = sw_center_mod_q(rand() % SW_KYBER_Q);
    }

    // Poison the output so an untouched slot cannot pass
    for (int i = 0; i < n_coeff; i++) h_r[i] = 0x7fff;

    std::cout << "synchronize input buffer data to device global memory\n";
    bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_r.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    std::cout << "Execution of the kernel\n";
    auto run = xrt::run(krnl);
    bind_run_arguments(run, bo_a, bo_b, bo_r, static_cast<unsigned>(batch),
                       using_batch_kernel);
    run.start();
    run.wait();

    std::cout << "Get the output data from the device\n";
    bo_r.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

    // Correctness. The kernel output is only lazily reduced, congruent mod q but not necessarily centered so compare as residues
    int16_t expected[N];
    int errors = 0;
    for (int n = 0; n < batch; n++) {
        sw_poly_mul_schoolbook(&h_a[n * N], &h_b[n * N], expected);
        for (int i = 0; i < N; i++) {
            if (sw_center_mod_q(h_r[n * N + i]) != expected[i]) {
                if (errors < 10)
                    printf("MISMATCH slot %d coeff %d: got %d (raw %d), expected %d\n",
                           n, i, sw_center_mod_q(h_r[n * N + i]), h_r[n * N + i],
                           expected[i]);
                errors++;
            }
        }
    }

    if (errors) {
        std::cout << "*******************************************\n";
        printf("FAIL: %d/%d coefficients mismatched across %d slot(s)\n",
               errors, batch * N, batch);
        std::cout << "*******************************************\n";
        return EXIT_FAILURE;
    }

    std::cout << "*******************************************\n";
    printf("PASS: all %d slot(s) match the schoolbook product\n", batch);
    std::cout << "*******************************************\n";

    /* Cross-check the software baseline as well */
    int16_t sw_check[N], expected0[N];
    sw_poly_mul_schoolbook(h_a, h_b, expected0);
    sw_poly_mul(h_a, h_b, sw_check);
    for (int i = 0; i < N; i++) {
        if (sw_center_mod_q(sw_check[i]) != expected0[i]) {
            printf("FAIL: software baseline disagrees with schoolbook at %d\n", i);
            return EXIT_FAILURE;
        }
    }
    std::cout << "software baseline verified against schoolbook product\n";

    // Profiling. Every series. four hardware, one software.
    // kernel only. inputs already in device memory
    Timings hw_krnl = time_series(50, num_trials, batch, [&](int) {
        run.start();
        run.wait();
    });

    // end-to-end -- what a caller with fresh operands costs
    Timings hw_e2e = time_series(0, num_trials, batch, [&](int) {
        bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        run.start();
        run.wait();
        bo_r.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    });

    /* Dispatch-vs-device decomposition.
       The CU is ap_ctrl_chain, so it accepts a new ap_start while a previous
       run is still in flight. Enqueueing k runs back-to-back and waiting once
       therefore costs roughly

           total(k) = fixed_overhead + k * device_time

       Fitting that line separates the per-dispatch software/driver cost from
       the time the kernel occupies the device 
       
       without needing to set up a second kernel or AXI profiling monitors
       slope is what to compare against the csynth latency estimate
       
       the intercept is the cost being amortised. */
    printf("\n  dispatch decomposition (k runs enqueued, one wait):\n");
    const int ks[] = {1, 2, 4, 8, 16};
    double t_k[5];
    for (int ki = 0; ki < 5; ki++) {
        const int k = ks[ki];
        double best = 1e30;
        for (int rep = 0; rep < 20; rep++) {
            std::vector<xrt::run> runs;
            runs.reserve(k);
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < k; i++) {
                xrt::run r(krnl);
                bind_run_arguments(r, bo_a, bo_b, bo_r,
                                   static_cast<unsigned>(batch),
                                   using_batch_kernel);
                r.start();
                runs.push_back(std::move(r));
            }
            for (auto &r : runs) r.wait();
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (us < best) best = us;
        }
        t_k[ki] = best;
        printf("    k=%2d  total %8.2f us   per dispatch %7.2f us\n",
               k, best, best / k);
    }
    {
        /* Least-squares slope/intercept over the five points. */
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < 5; i++) {
            sx += ks[i]; sy += t_k[i];
            sxx += (double)ks[i] * ks[i]; sxy += (double)ks[i] * t_k[i];
        }
        double slope = (5 * sxy - sx * sy) / (5 * sxx - sx * sx);
        double intercept = (sy - slope * sx) / 5;
        printf("    fit: device %7.2f us/dispatch, fixed overhead %7.2f us\n",
               slope, intercept);
        printf("         device per multiply: %6.3f us\n", slope / batch);
    }

    /* Independent ring slots. Each queued command owns distinct BOs, so this
       models real jobs and permits transfers for a completed slot while other
       jobs remain in flight. It also avoids overlapping commands racing on one
       shared output buffer object */
    Timings hw_pipe{}, hw_overlap{};
    {
        std::vector<std::unique_ptr<RingSlot>> ring;
        ring.reserve(queue_depth);
        for (int i = 0; i < queue_depth; ++i) {
            ring.emplace_back(std::make_unique<RingSlot>(
                device, krnl, buf_bytes, static_cast<unsigned>(batch),
                using_batch_kernel));
            initialize_ring_slot(*ring.back(), n_coeff, 0x9e3779b9u + i * 0x10001u);
            ring.back()->bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            ring.back()->bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            ring.back()->bo_r.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        }

        // Resident-buffer throughput: queue stays full and no sync is timed.
        for (int i = 0; i < queue_depth; ++i) ring[i]->run.start();
        hw_pipe = time_series(50, num_trials, batch, [&](int t) {
            RingSlot& slot = *ring[t % queue_depth];
            slot.run.wait();
            slot.run.start();
        });
        for (int i = 0; i < queue_depth; ++i) ring[i]->run.wait();

        int ring_errors = 0;
        for (int i = 0; i < queue_depth; ++i) {
            ring[i]->bo_r.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            ring_errors += verify_ring_slot(*ring[i], batch, i);
        }
        if (ring_errors) {
            printf("FAIL: independent ring verification found %d mismatches\n",
                   ring_errors);
            return EXIT_FAILURE;
        }
        printf("independent ring verified: %d slots, %d polynomials each\n",
               queue_depth, batch);

        /* Overlapped end-to-end throughput. Waiting for the oldest slot,
           retrieving it, refreshing its inputs, and resubmitting it occurs
           while the remaining slots stay queued on the accelerator*/
        for (int i = 0; i < queue_depth; ++i) ring[i]->run.start();
        hw_overlap = time_series(50, num_trials, batch, [&](int t) {
            RingSlot& slot = *ring[t % queue_depth];
            slot.run.wait();
            slot.bo_r.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            slot.bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            slot.bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            slot.run.start();
        });
        for (int i = 0; i < queue_depth; ++i) ring[i]->run.wait();
    }

    // software baseline on the A53
    int16_t sw_a[N], sw_b[N], sw_r[N];
    std::memcpy(sw_a, h_a, POLY_BYTES);
    std::memcpy(sw_b, h_b, POLY_BYTES);
    volatile int16_t sink = 0;  // keeps the optimiser from deleting the work

    for (int t = 0; t < 50; t++) { sw_poly_mul(sw_a, sw_b, sw_r); }  // warm up

    // software
    std::vector<double> sw_us;
    sw_us.reserve(num_trials);
    for (int t = 0; t < num_trials; t++) {
        auto t0 = std::chrono::steady_clock::now();
        sw_poly_mul(sw_a, sw_b, sw_r);
        auto t1 = std::chrono::steady_clock::now();
        sw_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        sink = (int16_t)(sink + sw_r[0]);
    }
    (void)sink;

    Timings sw = compute_stats(sw_us);

    printf("\n===== poly_mul: %d trials, batch %d, queue depth %d =====\n",
           num_trials, batch, queue_depth);
    printf("  all times are PER POLYNOMIAL MULTIPLY\n");
    printf("  hardware: %d multiplies (%d dispatches x %d per dispatch)\n",
           num_trials * batch, num_trials, batch);
    printf("  software: %d multiplies (1 per trial)\n", num_trials);
    printf("  %-22s %9s %9s %9s %9s %9s\n", "", "median", "mean", "min", "max", "stddev");
    printf("  %-22s %9s %9s %9s %9s %9s\n", "", "(us)", "(us)", "(us)", "(us)", "(us)");
    print_row("hardware (kernel)", hw_krnl);
    print_row("hardware (ring resident)", hw_pipe);
    print_row("hardware (ring e2e)", hw_overlap);
    print_row("hardware (serial e2e)", hw_e2e);
    print_row("software (A53)", sw);
    printf("\n  speedup, kernel only  (median): %6.2fx\n", sw.median_us / hw_krnl.median_us);
    printf("  speedup, ring resident (median): %6.2fx   (depth %d)\n",
           sw.median_us / hw_pipe.median_us, queue_depth);
    printf("  speedup, ring e2e      (median): %6.2fx\n",
           sw.median_us / hw_overlap.median_us);
    printf("  ring gain over serial kernel:    %6.2fx\n",
           hw_krnl.median_us / hw_pipe.median_us);
    printf("  speedup, serial e2e    (median): %6.2fx\n",
           sw.median_us / hw_e2e.median_us);
    printf("  serial sync overhead per multiply: %6.2f us\n",
           hw_e2e.median_us - hw_krnl.median_us);
    printf("  dispatch cost amortised over:   %6d multiplies\n", batch);
    printf("  throughput, serial e2e:         %6.0f multiplies/s\n",
           1.0e6 / hw_e2e.mean_us);
    printf("  throughput, ring resident:      %6.0f multiplies/s\n",
           1.0e6 / hw_pipe.mean_us);
    printf("  throughput, ring e2e:           %6.0f multiplies/s\n",
           1.0e6 / hw_overlap.mean_us);
    printf("  mean response, ring e2e batch:  %6.2f us\n",
           hw_overlap.mean_us * batch);
    printf("  note: queued completion medians are bursty; use mean throughput\n");
    printf("====================================================================\n");

    return EXIT_SUCCESS;
}
