/*
 * XRT host application for the poly_mul / poly_mul_batch kernel.
 *
 * Loads the xclbin, runs polynomial multiplication in Rq = Zq[X]/(X^256+1)
 * on the PL, checks against schoolbook on the A53, then profiles.
 *
 * Numbers reported:
 *   kernel      -- run.start() to run.wait(), buffers already resident
 *   pipelined   -- steady-state with several runs queued (needs ap_ctrl_chain)
 *   end-to-end  -- plus host/device syncs
 *   dispatch fit -- device time vs fixed overhead via least squares
 *
 * --batch / -b must match the kernel's POLY_MUL_BATCH (128 for the batch top).
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
#include <numeric>
#include <vector>

#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

#define KERNEL_NAME       "pqcrystals_kyber768_ref_poly_mul"
#define KERNEL_BATCH_NAME "pqcrystals_kyber768_ref_poly_mul_batch"

static const int N          = SW_KYBER_N;
static const size_t POLY_BYTES = N * sizeof(int16_t);

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

static void print_row(const char* label, const Timings& s) {
    printf("  %-22s %9.2f %9.2f %9.2f %9.2f %9.2f\n", label, s.median_us, s.mean_us,
           s.min_us, s.max_us, s.stddev_us);
}

static void bind_args(xrt::run& run, xrt::bo& a, xrt::bo& b, xrt::bo& r) {
    run.set_arg(0, a);
    run.set_arg(1, b);
    run.set_arg(2, r);
}

int main(int argc, char** argv) {
    sda::utils::CmdLineParser parser;

    parser.addSwitch("--xclbin_file", "-x", "input binary file string", "");
    parser.addSwitch("--device_id", "-d", "device index", "0");
    parser.addSwitch("--iterations", "-n", "timed dispatches per series", "1000");
    parser.addSwitch("--batch", "-b", "polynomials per dispatch; must match the "
                                      "kernel's POLY_MUL_BATCH", "128");
    parser.parse(argc, argv);

    std::string binaryFile = parser.value("xclbin_file");
    int device_index = stoi(parser.value("device_id"));
    int num_trials = stoi(parser.value("iterations"));
    int batch = stoi(parser.value("batch"));

    std::cout << std::unitbuf;

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
    if (batch < 1) {
        std::cerr << "error: --batch must be >= 1\n";
        return EXIT_FAILURE;
    }

    const int    n_coeff = batch * N;
    const size_t buf_bytes = n_coeff * sizeof(int16_t);

    xrt::device device;
    xrt::kernel krnl;
    try {
        std::cout << "Open the device " << device_index << std::endl;
        device = xrt::device(device_index);
        std::cout << "Load the xclbin " << binaryFile << std::endl;
        auto uuid = device.load_xclbin(binaryFile);
        try {
            std::cout << "Open the kernel " << KERNEL_BATCH_NAME << std::endl;
            krnl = xrt::kernel(device, uuid, KERNEL_BATCH_NAME);
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

    auto h_a = bo_a.map<int16_t*>();
    auto h_b = bo_b.map<int16_t*>();
    auto h_r = bo_r.map<int16_t*>();

    srand(42);
    for (int i = 0; i < n_coeff; i++) {
        h_a[i] = sw_center_mod_q(rand() % SW_KYBER_Q);
        h_b[i] = sw_center_mod_q(rand() % SW_KYBER_Q);
    }
    for (int i = 0; i < n_coeff; i++) h_r[i] = 0x7fff;

    std::cout << "synchronize input buffer data to device global memory\n";
    bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    std::cout << "Execution of the kernel\n";
    auto run = xrt::run(krnl);
    bind_args(run, bo_a, bo_b, bo_r);
    run.start();
    run.wait();

    std::cout << "Get the output data from the device\n";
    bo_r.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

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
        if (batch > 1)
            printf("  a wrong --batch looks exactly like this; check it matches "
                   "the kernel's POLY_MUL_BATCH\n");
        std::cout << "*******************************************\n";
        return EXIT_FAILURE;
    }

    std::cout << "*******************************************\n";
    printf("PASS: all %d slot(s) match the schoolbook product\n", batch);
    std::cout << "*******************************************\n";

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

    std::vector<double> hw_krnl_us, hw_e2e_us, sw_us;
    hw_krnl_us.reserve(num_trials);
    hw_e2e_us.reserve(num_trials);
    sw_us.reserve(num_trials);

    for (int t = 0; t < 50; t++) { run.start(); run.wait(); }

    for (int t = 0; t < num_trials; t++) {
        auto t0 = std::chrono::steady_clock::now();
        run.start();
        run.wait();
        auto t1 = std::chrono::steady_clock::now();
        hw_krnl_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / batch);
    }

    for (int t = 0; t < num_trials; t++) {
        auto t0 = std::chrono::steady_clock::now();
        bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        run.start();
        run.wait();
        bo_r.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        auto t1 = std::chrono::steady_clock::now();
        hw_e2e_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / batch);
    }

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
                xrt::run rr(krnl);
                bind_args(rr, bo_a, bo_b, bo_r);
                rr.start();
                runs.push_back(std::move(rr));
            }
            for (auto& rr : runs) rr.wait();
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (us < best) best = us;
        }
        t_k[ki] = best;
        printf("    k=%2d  total %8.2f us   per dispatch %7.2f us\n",
               k, best, best / k);
    }
    {
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

    const int DEPTH = 8;
    std::vector<double> hw_pipe_us;
    hw_pipe_us.reserve(num_trials);
    {
        std::vector<xrt::run> ring;
        ring.reserve(DEPTH);
        for (int i = 0; i < DEPTH; i++) {
            xrt::run rr(krnl);
            bind_args(rr, bo_a, bo_b, bo_r);
            ring.push_back(std::move(rr));
        }
        for (int i = 0; i < DEPTH; i++) ring[i].start();
        for (int t = 0; t < 50; t++) {
            ring[t % DEPTH].wait();
            ring[t % DEPTH].start();
        }
        for (int t = 0; t < num_trials; t++) {
            auto t0 = std::chrono::steady_clock::now();
            ring[t % DEPTH].wait();
            ring[t % DEPTH].start();
            auto t1 = std::chrono::steady_clock::now();
            hw_pipe_us.push_back(
                std::chrono::duration<double, std::micro>(t1 - t0).count() / batch);
        }
        for (int i = 0; i < DEPTH; i++) ring[i].wait();
    }

    int16_t sw_a[N], sw_b[N], sw_r[N];
    std::memcpy(sw_a, h_a, POLY_BYTES);
    std::memcpy(sw_b, h_b, POLY_BYTES);
    volatile int16_t sink = 0;

    for (int t = 0; t < 50; t++) { sw_poly_mul(sw_a, sw_b, sw_r); }

    for (int t = 0; t < num_trials; t++) {
        auto t0 = std::chrono::steady_clock::now();
        sw_poly_mul(sw_a, sw_b, sw_r);
        auto t1 = std::chrono::steady_clock::now();
        sw_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        sink = (int16_t)(sink + sw_r[0]);
    }
    (void)sink;

    Timings hw_krnl = compute_stats(hw_krnl_us);
    Timings hw_e2e  = compute_stats(hw_e2e_us);
    Timings hw_pipe = compute_stats(hw_pipe_us);
    Timings sw      = compute_stats(sw_us);

    printf("\n===== poly_mul: %d trials per series, batch %d =====\n",
           num_trials, batch);
    printf("  all times are PER POLYNOMIAL MULTIPLY\n");
    printf("  hardware: %d multiplies (%d dispatches x %d per dispatch)\n",
           num_trials * batch, num_trials, batch);
    printf("  software: %d multiplies (1 per trial)\n", num_trials);
    printf("  %-22s %9s %9s %9s %9s %9s\n", "", "median", "mean", "min", "max", "stddev");
    printf("  %-22s %9s %9s %9s %9s %9s\n", "", "(us)", "(us)", "(us)", "(us)", "(us)");
    print_row("hardware (kernel)", hw_krnl);
    print_row("hardware (pipelined)", hw_pipe);
    print_row("hardware (end-to-end)", hw_e2e);
    print_row("software (A53)", sw);
    printf("\n  speedup, kernel only  (median): %6.2fx\n", sw.median_us / hw_krnl.median_us);
    printf("  speedup, pipelined    (median): %6.2fx   (depth %d)\n",
           sw.median_us / hw_pipe.median_us, DEPTH);
    printf("  pipelining gain over serial:    %6.2fx\n",
           hw_krnl.median_us / hw_pipe.median_us);
    printf("  speedup, end-to-end   (median): %6.2fx\n", sw.median_us / hw_e2e.median_us);
    printf("  sync overhead, per multiply:    %6.2f us\n",
           hw_e2e.median_us - hw_krnl.median_us);
    printf("  dispatch cost amortised over:   %6d multiplies\n", batch);
    printf("  throughput, end-to-end:         %6.0f multiplies/s\n",
           1.0e6 / hw_e2e.mean_us);
    printf("====================================================================\n");

    return EXIT_SUCCESS;
}
