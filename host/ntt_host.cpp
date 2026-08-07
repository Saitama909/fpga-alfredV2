#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>

#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>

// Notes for myself so ik what's going on
// Generate polynomials cpu, XRT buffer, use AXI-Lite to tell FPGA where the addresses are
// Run FPGA accelerator, check until completion, read results, then run NTT on CPU and compare. 


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

// Bundling DRAM buffer, CPU pointer and the physical address on FPGA. 
struct RingBufferSlot {
    xrt::bo device_buffer;
    int16_t* host_memory_ptr;
    uint64_t physical_dram_address;
};

int main(int argc, char** argv) {
    std::string xclbin_path = (argc > 1) ? argv[1] : "ntt_dataflow_200mhz.xclbin";
    size_t total_jobs = (argc > 2) ? std::stoul(argv[2]) : 10000;

    std::cout << "=======================================================\n";
    std::cout << " Stage 3: Direct MMIO & Circular Ring Buffer FIFO      \n";
    std::cout << " Bitstream: " << xclbin_path << "\n";
    std::cout << " Workload : " << total_jobs << " NTT jobs\n";
    std::cout << "=======================================================\n";

    // Initialize XRT Device and Kernel
    xrt::device device(0);
    auto uuid = device.load_xclbin(xclbin_path);
    
    // For AXI-Lite control regs 
    xrt::ip ip_control(device, uuid, "ntt_accel");

    constexpr size_t BATCH_SIZE = 128;      // NTTs per batch 
    constexpr size_t RING_CAPACITY = 32;    // How many circular slots (tried 2,4,8,16,32,64)
    size_t slot_bytes = BATCH_SIZE * 256 * sizeof(int16_t); // 64 KB

    std::vector<RingBufferSlot> ring(RING_CAPACITY);
    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        ring[i].device_buffer = xrt::bo(device, slot_bytes, xrt::bo::flags::normal, 0);
        ring[i].host_memory_ptr = ring[i].device_buffer.map<int16_t*>(); // For virtual CPU pointer
        ring[i].physical_dram_address = ring[i].device_buffer.address(); // For DRAM HW address (all from struct)
    }


    // Multiples of 128 ignored but it's fine right now we can just put limitations or something 
    size_t total_batches = total_jobs / BATCH_SIZE;
    int verification_errors = 0;

    // For end to end time
    auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t batch_idx = 0; batch_idx < total_batches; ++batch_idx) {
        uint32_t current_slot = batch_idx % RING_CAPACITY;
        RingBufferSlot& slot = ring[current_slot];

        // for my ref, expected_outputs[5][17] means coeff 17 of NTT job 5
        std::vector<std::vector<int16_t>> expected_outputs(BATCH_SIZE, std::vector<int16_t>(256));

        // Populate CPU virtual memory pointer with 128 NTTs
        for (size_t k = 0; k < BATCH_SIZE; ++k) {
            for (int i = 0; i < 256; ++i) {
                int16_t sample_val = (i * 11 + batch_idx * 128 + k) % 3329;
                slot.host_memory_ptr[k * 256 + i] = sample_val;
                expected_outputs[k][i] = sample_val; // FPGA overwrites buffer so we need copy for CPU
            }
        }
        
        // Flush CPU cache lines (data written to DRAM)
        slot.device_buffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);


        // AXI Lite control regs 
        //    - Bit 0 (0x01): ap_start -> Writing 1 commands FPGA logic to START computation
        //    - Bit 1 (0x02): ap_done  -> Read as 1 when FPGA hardware finishes execution.
        //    - Bit 2 (0x04): ap_idle  -> Read as 1 when FPGA kernel is idle

        // Write physical DRAM address to registers 0x10 & 0x14 (low/high 32 bits respectively)
        ip_control.write_register(0x10, static_cast<uint32_t>(slot.physical_dram_address & 0xFFFFFFFF));
        ip_control.write_register(0x14, static_cast<uint32_t>(slot.physical_dram_address >> 32));
        
        // ap_start
        ip_control.write_register(0x00, 0x01);


        // while the ap_done bit isn't set yet we keep going
        while ((ip_control.read_register(0x00) & 0x02) == 0) {

        }

        // Get output back!
        slot.device_buffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

        // Verify against CPU reference NTT
        for (size_t k = 0; k < BATCH_SIZE; ++k) {
            ntt_ref(expected_outputs[k].data());
            for (int i = 0; i < 256; ++i) {
                if (slot.host_memory_ptr[k * 256 + i] != expected_outputs[k][i]) {
                    verification_errors++;
                    break;
                }
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time_us = std::chrono::duration<double, std::micro>(end_time - start_time).count();
    size_t executed_jobs = total_batches * BATCH_SIZE;

    std::cout << "\n=======================================================\n";
    std::cout << " Result : " << (verification_errors == 0 ? "PASS (100% Correct)" : "FAIL") << "\n";
    std::cout << " Latency with ring buffer : " << std::fixed << std::setprecision(3) << (total_time_us / executed_jobs) << " us / NTT\n";
    std::cout << " Sustained Throughput: " << std::setprecision(1) << (executed_jobs / (total_time_us / 1e6)) << " NTTs / sec\n";
    std::cout << "=======================================================\n";

    return 0;
}
