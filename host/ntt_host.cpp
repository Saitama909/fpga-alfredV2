#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>

// Software Reference NTT (Kyber-768)
static const int16_t zetas[128] = {
    -1044,  -758,   -64, -1011,  -313, -1009,   657, -1453,
     1078,  -554,  -242,   -56,  1023, -1198,   641,  1110,
      446,  1068,  -982,  -977,   118,  -266,   471,   -62,
    -1082, -1012,  -644,    67,   703,   725,  -702,  -923,
     -926,   461,  1214,   356, -1231,   276,   390,  -557,
     -733,   834,   604,   608,   838,  -539,  -408,  -152,
     -344,  -340,  1041,  -909,   891,   984,  -157, -1047,
      998, -1050,  -875,   767,  -522,   999,   607,  -665,
     1087, -1172,  -630,  -889,  1056,  -985,  -346,  1033,
     1163,  -237,  -239,   341,   813,   734,  -462,  -686,
    -1051,  -727,   709,   647,   920,  -908,   109,  -110,
      418,  -763,  -412,  -956,   478,   728,   185,   415,
      902,  -483,  1128,  1060,   515,  -741,  -384, -1141,
     -993,   919,  -805,   -18,  1049,  -352,   -97,  -809,
      870,   420,   526,  -678,   760,   265,   674,  -668,
     -820,   476,  1115,  -142, -1136,   229,  -538,  -962
};

static int16_t fqmul(int16_t a, int16_t b) {
    int32_t c = (int32_t)a * b;
    int16_t u = (int16_t)(c * 62209);
    int32_t t = (int32_t)u * 3329;
    int16_t res = (int16_t)((c - t) >> 16);
    return res;
}

static void ntt_ref(int16_t r[256]) {
    int len, start, j, k;
    int16_t zeta;
    k = 1;
    for (len = 128; len >= 2; len >>= 1) {
        for (start = 0; start < 256; start += 2 * len) {
            zeta = zetas[k++];
            for (j = start; j < start + len; ++j) {
                int16_t t = fqmul(zeta, r[j + len]);
                r[j + len] = r[j] - t;
                r[j] = r[j] + t;
            }
        }
    }
}

// Baseline Host Scheduler - we only do one polynomial at once. 
// Submits 1 polynomial at a time via synchronous xrt::run::wait() calls
int main(int argc, char** argv) {
    std::string xclbin_path = (argc > 1) ? argv[1] : "ntt_dataflow_200mhz.xclbin";
    size_t total_jobs = (argc > 2) ? std::stoul(argv[2]) : 1000;

    std::cout << "=======================================================\n";
    std::cout << " Unoptimized Baseline Synchronous Host Scheduler      \n";
    std::cout << " Target Bitstream: " << xclbin_path << "\n";
    std::cout << " Total Workload  : " << total_jobs << " NTT jobs\n";
    std::cout << "=======================================================\n";

    // Initialize XRT Device and Kernel
    xrt::device device(0);
    auto uuid = device.load_xclbin(xclbin_path);
    xrt::kernel kernel(device, uuid, "ntt_accel");

    // 2. Allocate buffer for 1 polynomial
    xrt::bo bo_in(device, 256 * sizeof(int16_t), kernel.group_id(0));
    int16_t* host_ptr = bo_in.map<int16_t*>();

    int errors = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    // 3. Synchronous loop submitting 1 NTT per driver call
    for (size_t job = 0; job < total_jobs; ++job) {
        int16_t expected[256];
        for (int i = 0; i < 256; ++i) {
            host_ptr[i] = (i * 13 + job) % 3329;
            expected[i] = host_ptr[i];
        }

        bo_in.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        // Blocks thread on Linux kernel driver context switches on every single call
        auto run = kernel(bo_in);
        run.wait();

        bo_in.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

        // Verify against CPU reference NTT
        ntt_ref(expected);
        for (int i = 0; i < 256; ++i) {
            if (host_ptr[i] != expected[i]) {
                errors++;
                break;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_us = std::chrono::duration<double, std::micro>(end_time - start_time).count();

    std::cout << "\n=======================================================\n";
    std::cout << " Result                      : " << (errors == 0 ? "PASS (100% Correct)" : "FAIL") << "\n";
    std::cout << " Baseline Synchronous Latency: " << (total_us / total_jobs) << " us / NTT\n";
    std::cout << " Sustained Throughput        : " << (total_jobs / (total_us / 1e6)) << " NTTs / sec\n";
    std::cout << "=======================================================\n";

    return 0;
}
