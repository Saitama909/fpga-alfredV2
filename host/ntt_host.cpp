#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

namespace {

constexpr int kN = 256;
constexpr int16_t kQ = 3329;
constexpr int16_t kQInv = -3327;
constexpr std::size_t kBufferBytes = kN * sizeof(int16_t);

// Kyber's NTT twiddle factors in Montgomery representation.
constexpr int16_t kZetas[128] = {
    -1044, -758,  -359,  -1517, 1493,  1422,  287,   202,
    -171,  622,   1577,  182,   962,   -1202, -1474, 1468,
    573,   -1325, 264,   383,   -829,  1458,  -1602, -130,
    -681,  1017,  732,   608,   -1542, 411,   -205,  -1571,
    1223,  652,   -552,  1015,  -1293, 1491,  -282,  -1544,
    516,   -8,    -320,  -666,  -1618, -1162, 126,   1469,
    -853,  -90,   -271,  830,   107,   -1421, -247,  -951,
    -398,  961,   -1508, -725,  448,   -1065, 677,   -1275,
    -1103, 430,   555,   843,   -1251, 871,   1550,  105,
    422,   587,   177,   -235,  -291,  -460,  1574,  1653,
    -246,  778,   1159,  -147,  -777,  1483,  -602,  1119,
    -1590, 644,   -872,  349,   418,   329,   -156,  -75,
    817,   1097,  603,   610,   1322,  -1285, -1465, 384,
    -1215, -136,  1218,  -1335, -874,  220,   -1187, -1659,
    -1185, -1530, -1278, 794,   -1510, -854,  -870,  478,
    -108,  -308,  996,   991,   958,   -1460, 1522,  1628,
};

int16_t montgomery_reduce(int32_t value) {
  // This is the same arithmetic used by the Kyber reference implementation.
  int16_t t = static_cast<int16_t>(value) * kQInv;
  t = static_cast<int16_t>(
      (value - static_cast<int32_t>(t) * kQ) >> 16);
  return t;
}

int16_t fqmul(int16_t a, int16_t b) {
  return montgomery_reduce(static_cast<int32_t>(a) * b);
}

void software_ntt(int16_t polynomial[kN]) {
  unsigned int twiddle_index = 1;

  for (unsigned int length = 128; length >= 2; length >>= 1) {
    unsigned int start = 0;

    while (start < kN) {
      const int16_t zeta = kZetas[twiddle_index++];
      const unsigned int end = start + length;

      for (unsigned int j = start; j < end; ++j) {
        const int16_t t = fqmul(zeta, polynomial[j + length]);
        const int16_t a = polynomial[j];
        polynomial[j] = static_cast<int16_t>(a + t);
        polynomial[j + length] = static_cast<int16_t>(a - t);
      }

      start = end + length;
    }
  }
}

void initialise_input(int16_t polynomial[kN]) {
  for (int i = 0; i < kN; ++i) {
    polynomial[i] = static_cast<int16_t>(i - 128);
  }
}

unsigned int parse_run_count(const char* text) {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);

  if (text[0] == '\0' || end == nullptr || *end != '\0' ||
      value == 0 || value > 1000000UL) {
    throw std::invalid_argument(
        "run count must be an integer from 1 to 1000000");
  }

  return static_cast<unsigned int>(value);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: " << argv[0]
              << " <ntt.xclbin> [run_count] [kernel_name]\n"
              << "Example: " << argv[0]
              << " binary_container_1.xclbin 100 ntt_accel\n";
    return EXIT_FAILURE;
  }

  const std::string xclbin_path = argv[1];

  try {
    const unsigned int run_count =
        (argc >= 3) ? parse_run_count(argv[2]) : 1;
    const std::string kernel_name =
        (argc >= 4) ? argv[3] : "ntt_accel";

    std::cout << "Opening XRT device 0\n";
    xrt::device device(0);

    std::cout << "Loading " << xclbin_path << '\n';
    const auto uuid = device.load_xclbin(xclbin_path);

    std::cout << "Opening kernel " << kernel_name << '\n';
    xrt::kernel kernel(device, uuid, kernel_name);

    // Argument 0 is int16_t r[256]. group_id(0) chooses DDR memory that is
    // actually connected to this kernel argument in the linked system.
    xrt::bo buffer(device, kBufferBytes, kernel.group_id(0));
    int16_t* hardware_output = buffer.map<int16_t*>();

    int16_t input[kN];
    int16_t expected[kN];
    initialise_input(input);
    std::copy(input, input + kN, expected);
    software_ntt(expected);

    double total_microseconds = 0.0;

    for (unsigned int run_index = 0; run_index < run_count; ++run_index) {
      // ntt_accel operates in place, so restore the original input every run.
      std::copy(input, input + kN, hardware_output);

      const auto start = std::chrono::steady_clock::now();

      // Flush PS-side writes so the PL AXI master sees the input in DDR.
      buffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);

      // XRT writes the buffer address through AXI-Lite and asserts ap_start.
      xrt::run run = kernel(buffer);
      run.wait();

      // Invalidate/update the PS-side view after the PL overwrites the buffer.
      buffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

      const auto finish = std::chrono::steady_clock::now();
      total_microseconds +=
          std::chrono::duration<double, std::micro>(finish - start).count();

      for (int i = 0; i < kN; ++i) {
        if (hardware_output[i] != expected[i]) {
          std::cerr << "FAIL on run " << (run_index + 1)
                    << ", coefficient " << i
                    << ": hardware=" << hardware_output[i]
                    << ", expected=" << expected[i] << '\n';
          return EXIT_FAILURE;
        }
      }
    }

    std::cout << "PASS: hardware NTT matches the software reference for all "
              << kN << " coefficients across " << run_count << " run(s)\n";
    std::cout << std::fixed << std::setprecision(2)
              << "Average end-to-end time: "
              << (total_microseconds / run_count) << " us\n"
              << "Includes TO_DEVICE sync, kernel launch/wait, and "
                 "FROM_DEVICE sync\n";

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
