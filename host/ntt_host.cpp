#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>

// Kyber-768 NTT Constant Zeta Table
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

// Batch Payload buffer with 128 NTTs per batch / 64k
int main(int argc, char** argv) {
    std::string xclbin_path = (argc > 1) ? argv[1] : "ntt_dataflow_200mhz.xclbin";
    size_t total_jobs = (argc > 2) ? std::stoul(argv[2]) : 10000;

    std::cout << "=======================================================\n";
    std::cout << " Target Bitstream: " << xclbin_path << "\n";
    std::cout << " Total Workload  : " << total_jobs << " NTT jobs\n";
    std::cout << " Batch Size      : 128 NTTs per payload (64 KB)\n";
    std::cout << "=======================================================\n";

    // Initialize XRT Device and Kernel
    xrt::device device(0);
    auto uuid = device.load_xclbin(xclbin_path);
    xrt::kernel kernel(device, uuid, "ntt_accel");

    // Allocate contiguous 64KB buffer for 128 NTTs
    size_t batch_size = 128;
    size_t batch_bytes = batch_size * 256 * sizeof(int16_t);
    xrt::bo bo_batch(device, batch_bytes, xrt::bo::flags::normal, kernel.group_id(0));
    int16_t* batch_ptr = bo_batch.map<int16_t*>();

    size_t num_batches = total_jobs / batch_size;
    int errors = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t b = 0; b < num_batches; ++b) {
        std::vector<std::vector<int16_t>> expected_batch(batch_size, std::vector<int16_t>(256));

        // Populate the NTTs
        for (size_t k = 0; k < batch_size; ++k) {
            for (int i = 0; i < 256; ++i) {
                int16_t val = (i * 7 + b * 128 + k) % 3329;
                batch_ptr[k * 256 + i] = val;
                expected_batch[k][i] = val;
            }
        }

        bo_batch.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        // Submit 128 NTTs in one go so we save all that overhead
        auto run = kernel(bo_batch);
        run.wait();

        bo_batch.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

        // Verify against CPU reference NTT
        for (size_t k = 0; k < batch_size; ++k) {
            ntt_ref(expected_batch[k].data());
            for (int i = 0; i < 256; ++i) {
                if (batch_ptr[k * 256 + i] != expected_batch[k][i]) {
                    errors++;
                    break;
                }
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_us = std::chrono::duration<double, std::micro>(end_time - start_time).count();
    size_t executed_jobs = num_batches * batch_size;

    std::cout << "\n=======================================================\n";
    std::cout << " Result               : " << (errors == 0 ? "PASS (100% Correct)" : "FAIL") << "\n";
    std::cout << " Latency with 128NTT batched   : " << std::fixed << std::setprecision(3) << (total_us / executed_jobs) << " us / NTT\n";
    std::cout << " Sustained Throughput : " << std::setprecision(1) << (executed_jobs / (total_us / 1e6)) << " NTTs / sec\n";
    std::cout << "=======================================================\n";

    return 0;
}
