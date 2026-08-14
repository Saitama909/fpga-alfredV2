#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>

#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>
#include <xrt/experimental/xrt_ip.h>


// Kyber-768 NTT Constant Zeta Table (matched with hls/src/ntt_top.cpp)
static const int16_t zetas[128] = {
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

// Bundling DRAM buffer, CPU pointer and the physical address on FPGA.
struct RingBufferSlot {
    xrt::bo device_buffer;
    int16_t* host_memory_ptr;
    uint64_t physical_dram_address;
};

int main(int argc, char** argv) {
    std::string xclbin_path = (argc > 1) ? argv[1] : "ntt_fresh.xclbin";
    size_t total_jobs = (argc > 2) ? std::stoul(argv[2]) : 10240;

    constexpr size_t BATCH_SIZE = 128;      // NTTs per batch
    constexpr size_t RING_CAPACITY = 32;    // Circular slots

    if (total_jobs % BATCH_SIZE != 0) {
        std::cerr << "ERROR: total_jobs must be a multiple of " << BATCH_SIZE << "\n";
        return 1;
    }

    std::cout << "=======================================================\n";
    std::cout << " Batched NTT Accelerator Host                          \n";
    std::cout << " Bitstream: " << xclbin_path << "\n";
    std::cout << " Workload : " << total_jobs << " NTT jobs (" << (total_jobs / BATCH_SIZE) << " batches)\n";
    std::cout << "=======================================================\n";

    // Initialize XRT Device and Kernel
    xrt::device device(0);
    auto uuid = device.load_xclbin(xclbin_path);

    // For AXI-Lite control regs
    xrt::ip ip_control(device, uuid, "ntt_accel");

    size_t slot_bytes = BATCH_SIZE * 256 * sizeof(int16_t); // 64 KB

    std::vector<RingBufferSlot> ring(RING_CAPACITY);
    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        ring[i].device_buffer = xrt::bo(device, slot_bytes, xrt::bo::flags::normal, 0);
        ring[i].host_memory_ptr = ring[i].device_buffer.map<int16_t*>();
        ring[i].physical_dram_address = ring[i].device_buffer.address();
    }

    size_t total_batches = total_jobs / BATCH_SIZE;
    int verification_errors = 0;

    double total_cpu_us = 0.0;
    double total_e2e_us = 0.0;
    double total_kernel_us = 0.0;

    for (size_t batch_idx = 0; batch_idx < total_batches; ++batch_idx) {
        uint32_t current_slot = batch_idx % RING_CAPACITY;
        RingBufferSlot& slot = ring[current_slot];

        // ------------------------------------------------
        // PREPARE INPUT — NOT TIMED FOR ACCELERATOR
        // ------------------------------------------------
        std::vector<int16_t> expected(BATCH_SIZE * 256);

        for (size_t k = 0; k < BATCH_SIZE; ++k) {
            for (int i = 0; i < 256; ++i) {
                int16_t sample_val = (i * 11 + batch_idx * BATCH_SIZE + k) % 3329;
                size_t idx = k * 256 + i;
                slot.host_memory_ptr[idx] = sample_val;
                expected[idx] = sample_val;
            }
        }

        // Calculate CPU expected values (timed for CPU baseline comparison)
        auto cpu_start = std::chrono::high_resolution_clock::now();
        for (size_t k = 0; k < BATCH_SIZE; ++k) {
            ntt_ref(expected.data() + k * 256);
        }
        auto cpu_end = std::chrono::high_resolution_clock::now();
        total_cpu_us += std::chrono::duration<double, std::micro>(cpu_end - cpu_start).count();

        // ------------------------------------------------
        // END-TO-END ACCELERATOR TIMER
        // ------------------------------------------------
        auto e2e_start = std::chrono::high_resolution_clock::now();

        slot.device_buffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        // Tell kernel which BO contains the entire batch.
        ip_control.write_register(
            0x10,
            static_cast<uint32_t>(slot.physical_dram_address & 0xFFFFFFFF)
        );

        ip_control.write_register(
            0x14,
            static_cast<uint32_t>(slot.physical_dram_address >> 32)
        );

        // ------------------------------------------------
        // KERNEL-ONLY TIMER
        // ------------------------------------------------
        auto kernel_start = std::chrono::high_resolution_clock::now();

        ip_control.write_register(0x00, 0x01);

        while ((ip_control.read_register(0x00) & 0x02) == 0) {
        }

        auto kernel_end = std::chrono::high_resolution_clock::now();

        slot.device_buffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

        auto e2e_end = std::chrono::high_resolution_clock::now();

        total_kernel_us += std::chrono::duration<double, std::micro>(
            kernel_end - kernel_start
        ).count();

        total_e2e_us += std::chrono::duration<double, std::micro>(
            e2e_end - e2e_start
        ).count();

        // ------------------------------------------------
        // VERIFY — NOT TIMED
        // ------------------------------------------------
        for (size_t k = 0; k < BATCH_SIZE; ++k) {
            for (int i = 0; i < 256; ++i) {
                size_t idx = k * 256 + i;
                if (slot.host_memory_ptr[idx] != expected[idx]) {
                    verification_errors++;
                    break;
                }
            }
        }
    }

    size_t executed_jobs = total_batches * BATCH_SIZE;
    double cpu_lat = total_cpu_us / executed_jobs;
    double kernel_lat = total_kernel_us / executed_jobs;
    double e2e_lat = total_e2e_us / executed_jobs;

    std::cout << "\n=======================================================\n";
    std::cout << " Result: " << (verification_errors == 0 ? "PASS" : "FAIL") << "\n";
    std::cout << " Batch size: " << BATCH_SIZE << "\n";
    std::cout << " CPU reference latency per NTT: " << std::fixed << std::setprecision(3) << cpu_lat << " us\n";
    std::cout << " Kernel/control latency per NTT: " << kernel_lat << " us\n";
    std::cout << " Transfer + kernel latency per NTT: " << e2e_lat << " us\n";
    std::cout << " Kernel-only Speedup vs CPU: " << std::setprecision(2) << (total_cpu_us / total_kernel_us) << "x\n";
    std::cout << " End-to-End Speedup vs CPU: " << (total_cpu_us / total_e2e_us) << "x\n";
    std::cout << " End-to-end accelerator throughput: " << std::setprecision(1) << (executed_jobs / (total_e2e_us / 1e6)) << " NTT/s\n";
    std::cout << "=======================================================\n";

    return (verification_errors == 0 ? 0 : 1);
}


