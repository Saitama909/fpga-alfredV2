/*
 * XRT host application for the kyber_enc_core kernel.
 *
 * Loads the xclbin, runs one Kyber IND-CPA encryption (the NTT-domain stage:
 * u = A^T.r + e1, v = t^T.r + e2 + m) on the PL, checks the result against a
 * software encryption computed on the A53, then profiles the two against each
 * other with the same timing code.
 *
 * See docs/kyber_enc_verification.md for stimulus generation and accuracy checks.
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

#define KERNEL_NAME "kyber_enc_core"

#define KYBER_K 3
static const int    N          = SW_KYBER_N;
static const int    Q          = SW_KYBER_Q;
static const size_t POLY_BYTES = N * sizeof(int16_t);

static const int16_t host_zetas[128] = {
  -1044,  -758,  -359, -1517,  1493,  1422,   287,   202,
   -171,   622,  1577,   182,   962, -1202, -1474,  1468,
    573, -1325,   264,   383,  -829,  1458, -1602,  -130,
   -681,  1017,   732,   608, -1542,   411,  -205, -1571,
   1223,   652,  -552,  1015, -1293,  1491,  -282, -1544,
    516,    -8,  -320,  -666, -1618, -1162,   126,  1469,
   -853,   -90,  -271,   830,   107, -1421,  -247,  -951,
   -398,   961, -1508,  -725,   448, -1065,   677, -1275,
  -1103,   430,   555,   843, -1251,   871,  1550,   105,
    422,   587,   177,  -235,  -291,  -460,  1574,  1653,
   -246,   778,  1159,  -147,  -777,  1483,  -602,  1119,
  -1590,   644,  -872,   349,   418,   329,  -156,   -75,
    817,  1097,   603,   610,  1322, -1285, -1465,   384,
  -1215,  -136,  1218, -1335,  -874,   220, -1187, -1659,
  -1185, -1530, -1278,   794, -1510,  -854,  -870,   478,
   -108,  -308,   996,   991,   958, -1460,  1522,  1628
};

static int16_t host_mont_reduce(int32_t a) {
    int16_t t = (int16_t)((int16_t)a * (int16_t)(-3327));
    return (int16_t)((a - (int32_t)t * Q) >> 16);
}

static int16_t host_barrett(int16_t a) {
    const int16_t v = (int16_t)(((1l << 26) + Q / 2) / Q);
    int16_t t = (int16_t)(((int32_t)v * a + (1l << 25)) >> 26);
    return (int16_t)(a - t * Q);
}

static int16_t host_fqmul(int16_t a, int16_t b) {
    return host_mont_reduce((int32_t)a * b);
}

static void host_ntt(int16_t r[256]) {
    unsigned k = 1;
    for (int len = 128; len >= 2; len >>= 1) {
        for (int start = 0; start < 256; start += 2 * len) {
            int16_t zeta = host_zetas[k++];
            for (int j = start; j < start + len; j++) {
                int16_t t = host_fqmul(zeta, r[j + len]);
                r[j + len] = (int16_t)(r[j] - t);
                r[j]       = (int16_t)(r[j] + t);
            }
        }
    }
    for (int i = 0; i < 256; i++) r[i] = host_barrett(r[i]);
}

static void sw_kyber_enc(const int16_t A_T[KYBER_K][KYBER_K][256],
                         const int16_t t[KYBER_K][256],
                         const int16_t r[KYBER_K][256],
                         const int16_t e1[KYBER_K][256],
                         const int16_t e2[256],
                         const int16_t msg[256],
                         int16_t u[KYBER_K][256],
                         int16_t v[256]) {
    int16_t prod[256];

    for (int i = 0; i < KYBER_K; i++) {
        for (int c = 0; c < N; c++) u[i][c] = 0;
        for (int j = 0; j < KYBER_K; j++) {
            sw_poly_mul(A_T[i][j], r[j], prod);
            for (int c = 0; c < N; c++)
                u[i][c] = sw_center_mod_q(u[i][c] + prod[c]);
        }
        for (int c = 0; c < N; c++)
            u[i][c] = sw_center_mod_q(u[i][c] + e1[i][c]);
    }

    for (int c = 0; c < N; c++) v[c] = 0;
    for (int j = 0; j < KYBER_K; j++) {
        sw_poly_mul(t[j], r[j], prod);
        for (int c = 0; c < N; c++)
            v[c] = sw_center_mod_q(v[c] + prod[c]);
    }
    for (int c = 0; c < N; c++)
        v[c] = sw_center_mod_q(v[c] + e2[c] + msg[c]);
}

struct Timings { double min_us, max_us, mean_us, median_us, stddev_us; };

static Timings compute_stats(std::vector<double> samples_us) {
    Timings s{};
    std::sort(samples_us.begin(), samples_us.end());
    size_t n = samples_us.size();
    s.min_us = samples_us.front();
    s.max_us = samples_us.back();
    double sum = std::accumulate(samples_us.begin(), samples_us.end(), 0.0);
    s.mean_us = sum / n;
    s.median_us = (n % 2 == 0) ? (samples_us[n/2 - 1] + samples_us[n/2]) / 2.0
                               : samples_us[n/2];
    double sq_sum = 0.0;
    for (double x : samples_us) sq_sum += (x - s.mean_us) * (x - s.mean_us);
    s.stddev_us = std::sqrt(sq_sum / n);
    return s;
}

static void print_row(const char* label, const Timings& s) {
    printf("  %-22s %9.2f %9.2f %9.2f %9.2f %9.2f\n", label, s.median_us,
           s.mean_us, s.min_us, s.max_us, s.stddev_us);
}

static void host_cbd(int16_t p[256]) {
    for (int i = 0; i < N; i++) {
        int acc = 0;
        for (int k = 0; k < 2; k++) acc += (rand() & 1) - (rand() & 1);
        p[i] = (int16_t)acc;
    }
}

static void bind_args(xrt::run& run, xrt::bo& A, xrt::bo& t, xrt::bo& r,
                      xrt::bo& e1, xrt::bo& e2, xrt::bo& msg,
                      xrt::bo& u, xrt::bo& v) {
    run.set_arg(0, A);
    run.set_arg(1, t);
    run.set_arg(2, r);
    run.set_arg(3, e1);
    run.set_arg(4, e2);
    run.set_arg(5, msg);
    run.set_arg(6, u);
    run.set_arg(7, v);
}

int main(int argc, char** argv) {
    sda::utils::CmdLineParser parser;
    parser.addSwitch("--xclbin_file", "-x", "input binary file string", "");
    parser.addSwitch("--device_id", "-d", "device index", "0");
    parser.addSwitch("--iterations", "-n", "timed dispatches per series", "1000");
    parser.parse(argc, argv);

    std::string binaryFile = parser.value("xclbin_file");
    int device_index = stoi(parser.value("device_id"));
    int num_trials   = stoi(parser.value("iterations"));

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

    xrt::device device;
    xrt::kernel krnl;
    try {
        std::cout << "Open the device " << device_index << std::endl;
        device = xrt::device(device_index);
        std::cout << "Load the xclbin " << binaryFile << std::endl;
        auto uuid = device.load_xclbin(binaryFile);
        std::cout << "Open the kernel " << KERNEL_NAME << std::endl;
        krnl = xrt::kernel(device, uuid, KERNEL_NAME);
    } catch (const std::exception& e) {
        std::cerr << "XRT error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Allocate Buffers in Global Memory\n";
    auto bo_A   = xrt::bo(device, KYBER_K * KYBER_K * POLY_BYTES, krnl.group_id(0));
    auto bo_t   = xrt::bo(device, KYBER_K * POLY_BYTES,           krnl.group_id(1));
    auto bo_r   = xrt::bo(device, KYBER_K * POLY_BYTES,           krnl.group_id(2));
    auto bo_e1  = xrt::bo(device, KYBER_K * POLY_BYTES,           krnl.group_id(3));
    auto bo_e2  = xrt::bo(device, POLY_BYTES,                     krnl.group_id(4));
    auto bo_msg = xrt::bo(device, POLY_BYTES,                     krnl.group_id(5));
    auto bo_u   = xrt::bo(device, KYBER_K * POLY_BYTES,           krnl.group_id(6));
    auto bo_v   = xrt::bo(device, POLY_BYTES,                     krnl.group_id(7));

    auto h_A   = bo_A.map<int16_t*>();
    auto h_t   = bo_t.map<int16_t*>();
    auto h_r   = bo_r.map<int16_t*>();
    auto h_e1  = bo_e1.map<int16_t*>();
    auto h_e2  = bo_e2.map<int16_t*>();
    auto h_msg = bo_msg.map<int16_t*>();
    auto h_u   = bo_u.map<int16_t*>();
    auto h_v   = bo_v.map<int16_t*>();

    srand(42);

    static int16_t A_n[KYBER_K][KYBER_K][256], t_n[KYBER_K][256];
    static int16_t r_n[KYBER_K][256], e1_n[KYBER_K][256], e2_n[256], msg_n[256];

    for (int i = 0; i < KYBER_K; i++)
        for (int j = 0; j < KYBER_K; j++)
            for (int c = 0; c < N; c++)
                A_n[i][j][c] = sw_center_mod_q(rand() % Q);
    for (int j = 0; j < KYBER_K; j++)
        for (int c = 0; c < N; c++)
            t_n[j][c] = sw_center_mod_q(rand() % Q);

    for (int j = 0; j < KYBER_K; j++) { host_cbd(r_n[j]); host_cbd(e1_n[j]); }
    host_cbd(e2_n);
    for (int c = 0; c < N; c++)
        msg_n[c] = (rand() & 1) ? (int16_t)((Q + 1) / 2) : 0;

    for (int i = 0; i < KYBER_K; i++)
        for (int j = 0; j < KYBER_K; j++) {
            std::memcpy(&h_A[(i * KYBER_K + j) * N], A_n[i][j], POLY_BYTES);
            host_ntt(&h_A[(i * KYBER_K + j) * N]);
        }
    for (int j = 0; j < KYBER_K; j++) {
        std::memcpy(&h_t[j * N], t_n[j], POLY_BYTES);
        host_ntt(&h_t[j * N]);
        std::memcpy(&h_r[j * N],  r_n[j],  POLY_BYTES);
        std::memcpy(&h_e1[j * N], e1_n[j], POLY_BYTES);
    }
    std::memcpy(h_e2,  e2_n,  POLY_BYTES);
    std::memcpy(h_msg, msg_n, POLY_BYTES);

    for (int i = 0; i < KYBER_K * N; i++) h_u[i] = 0x7fff;
    for (int i = 0; i < N; i++)           h_v[i] = 0x7fff;

    std::cout << "synchronize input buffers to device global memory\n";
    bo_A.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_t.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_r.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_e1.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_e2.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_msg.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    std::cout << "Execution of the kernel\n";
    auto run = xrt::run(krnl);
    bind_args(run, bo_A, bo_t, bo_r, bo_e1, bo_e2, bo_msg, bo_u, bo_v);
    run.start();
    run.wait();

    std::cout << "Get the output data from the device\n";
    bo_u.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    bo_v.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

    static int16_t u_ref[KYBER_K][256], v_ref[256];
    sw_kyber_enc(A_n, t_n, r_n, e1_n, e2_n, msg_n, u_ref, v_ref);

    int errors = 0;
    for (int i = 0; i < KYBER_K; i++)
        for (int c = 0; c < N; c++)
            if (sw_center_mod_q(h_u[i * N + c]) != u_ref[i][c]) {
                if (errors < 10)
                    printf("MISMATCH u[%d][%3d]: got %6d (raw %6d), expected %6d\n",
                           i, c, sw_center_mod_q(h_u[i * N + c]),
                           h_u[i * N + c], u_ref[i][c]);
                errors++;
            }
    for (int c = 0; c < N; c++)
        if (sw_center_mod_q(h_v[c]) != v_ref[c]) {
            if (errors < 10)
                printf("MISMATCH v[%3d]: got %6d (raw %6d), expected %6d\n",
                       c, sw_center_mod_q(h_v[c]), h_v[c], v_ref[c]);
            errors++;
        }

    if (errors) {
        std::cout << "*******************************************\n";
        printf("FAIL: %d/%d coefficients mismatched\n", errors, (KYBER_K + 1) * N);
        std::cout << "*******************************************\n";
        return EXIT_FAILURE;
    }

    std::cout << "*******************************************\n";
    printf("PASS: u and v match the software encryption\n");
    std::cout << "*******************************************\n";

    {
        int16_t school[256], viantt[256];
        sw_poly_mul_schoolbook(A_n[0][0], r_n[0], school);
        sw_poly_mul(A_n[0][0], r_n[0], viantt);
        for (int i = 0; i < N; i++)
            if (sw_center_mod_q(viantt[i]) != school[i]) {
                printf("FAIL: software baseline disagrees with schoolbook at %d\n", i);
                return EXIT_FAILURE;
            }
        std::cout << "software baseline verified against schoolbook product\n";
    }

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
        hw_krnl_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    for (int t = 0; t < num_trials; t++) {
        auto t0 = std::chrono::steady_clock::now();
        bo_r.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_e1.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_e2.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_msg.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        run.start();
        run.wait();
        bo_u.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        bo_v.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        auto t1 = std::chrono::steady_clock::now();
        hw_e2e_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    /* Dispatch fit: total(k) ≈ overhead + k * device_time */
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
                bind_args(rr, bo_A, bo_t, bo_r, bo_e1, bo_e2, bo_msg, bo_u, bo_v);
                rr.start();
                runs.push_back(std::move(rr));
            }
            for (auto& rr : runs) rr.wait();
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (us < best) best = us;
        }
        t_k[ki] = best;
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < 5; i++) {
        sx += ks[i]; sy += t_k[i];
        sxx += (double)ks[i] * ks[i]; sxy += (double)ks[i] * t_k[i];
    }
    double device_us = (5 * sxy - sx * sy) / (5 * sxx - sx * sx);
    double overhead_us = (sy - device_us * sx) / 5;

    const int DEPTH = 8;
    std::vector<double> hw_pipe_us;
    hw_pipe_us.reserve(num_trials);
    {
        std::vector<xrt::run> ring;
        ring.reserve(DEPTH);
        for (int i = 0; i < DEPTH; i++) {
            xrt::run rr(krnl);
            bind_args(rr, bo_A, bo_t, bo_r, bo_e1, bo_e2, bo_msg, bo_u, bo_v);
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
                std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        for (int i = 0; i < DEPTH; i++) ring[i].wait();
    }

    static int16_t u_sw[KYBER_K][256], v_sw[256];
    volatile int16_t sink = 0;

    for (int t = 0; t < 20; t++)
        sw_kyber_enc(A_n, t_n, r_n, e1_n, e2_n, msg_n, u_sw, v_sw);

    for (int t = 0; t < num_trials; t++) {
        auto t0 = std::chrono::steady_clock::now();
        sw_kyber_enc(A_n, t_n, r_n, e1_n, e2_n, msg_n, u_sw, v_sw);
        auto t1 = std::chrono::steady_clock::now();
        sw_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        sink = (int16_t)(sink + v_sw[0]);
    }
    (void)sink;

    Timings hw_krnl = compute_stats(hw_krnl_us);
    Timings hw_e2e  = compute_stats(hw_e2e_us);
    Timings hw_pipe = compute_stats(hw_pipe_us);
    Timings sw      = compute_stats(sw_us);

    printf("\n===== kyber_enc_core: %d trials per series, K = %d =====\n",
           num_trials, KYBER_K);
    printf("  all times are PER ENCRYPTION (%d polynomial products each)\n",
           KYBER_K * (KYBER_K + 1));
    printf("  %-22s %9s %9s %9s %9s %9s\n", "", "median", "mean", "min", "max", "stddev");
    printf("  %-22s %9s %9s %9s %9s %9s\n", "", "(us)", "(us)", "(us)", "(us)", "(us)");
    print_row("hardware (kernel)", hw_krnl);
    print_row("hardware (pipelined)", hw_pipe);
    print_row("hardware (end-to-end)", hw_e2e);
    print_row("software (A53)", sw);

    printf("\n  --- speedup vs software ---\n");
    printf("  speedup, kernel only  (median): %6.2fx\n",
           sw.median_us / hw_krnl.median_us);
    printf("  speedup, pipelined    (median): %6.2fx   (depth %d)\n",
           sw.median_us / hw_pipe.median_us, DEPTH);
    printf("  speedup, end-to-end   (median): %6.2fx\n",
           sw.median_us / hw_e2e.median_us);

    printf("\n  --- throughput ---\n");
    printf("  FPGA pipelined:                 %8.0f encryptions/s\n",
           1.0e6 / hw_pipe.median_us);
    printf("  FPGA end-to-end:                %8.0f encryptions/s\n",
           1.0e6 / hw_e2e.median_us);
    printf("  software (A53):                 %8.0f encryptions/s\n",
           1.0e6 / sw.median_us);

    printf("\n  --- detail ---\n");
    printf("  device time (dispatch fit):     %8.2f us/encryption\n", device_us);
    printf("  fixed XRT overhead (fit):       %8.2f us\n", overhead_us);
    printf("  sync overhead (e2e - kernel):   %8.2f us\n",
           hw_e2e.median_us - hw_krnl.median_us);
    printf("  pipelining gain over serial:    %6.2fx\n",
           hw_krnl.median_us / hw_pipe.median_us);
    printf("====================================================================\n");

    return EXIT_SUCCESS;
}
