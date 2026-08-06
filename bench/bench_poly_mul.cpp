/*
 * Software baseline for one polynomial multiplication in Rq = Zq[X]/(X^256+1).
 *
 * Links against the unmodified pq-crystals reference (ntt.c, reduce.c), built
 * as C and linked in, not the restructured HLS source. 
 * 
 * Computes exactly what the poly_mul kernel computes:
 *
 *     ntt(a); ntt(b); pointwise basemul; invntt_tomont  ==  a * b in Rq
 *
 * basemul contributes R^-1 and invntt contributes R, so the Montgomery factors
 * cancel and the result is the plain product mod q.
 *
 * C++ with std::chrono so that we are timing both the hardware and software implementations with the same timing libraries.
 * 
 *
 * Build: see Makefile (needs a pq-crystals source code).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <array>
#include <chrono>

/* The reference code is in C */
extern "C" {
#include "params.h"
#include "ntt.h"
#include "reduce.h"
}

constexpr int ITERS = 1000;  // multiplies per timed batch
constexpr int RUNS  = 21;    // batches; median is reported

using clk = std::chrono::steady_clock;

/* helpers */

static int16_t center_mod_q(int64_t x) {
  int64_t r = x % KYBER_Q;
  if (r < 0) r += KYBER_Q;
  if (r > KYBER_Q / 2) r -= KYBER_Q;
  return static_cast<int16_t>(r);
}

/* the measured work */

/* Reference poly_basemul_montgomery, inlined here so we only need to link
   ntt.c and reduce.c rather than pulling in poly.c and its dependencies. */
static void ref_basemul_all(int16_t r[256], const int16_t a[256], const int16_t b[256]) {
  for (unsigned int i = 0; i < KYBER_N / 4; i++) {
    basemul(&r[4*i],   &a[4*i],   &b[4*i],    zetas[64 + i]);
    basemul(&r[4*i+2], &a[4*i+2], &b[4*i+2], static_cast<int16_t>(-zetas[64 + i]));
  }
}

/* One full multiply. The two input copies included deliberately to match the hardware implementation, but when timed have no impact */
static void sw_poly_mul(const int16_t a[256], const int16_t b[256], int16_t r[256]) {
  int16_t ta[256], tb[256];
  std::memcpy(ta, a, sizeof ta);
  std::memcpy(tb, b, sizeof tb);
  ntt(ta);
  ntt(tb);
  ref_basemul_all(r, ta, tb);
  invntt(r);
}

/* textbook_version: negacyclic convolution. acts as both the correctness reference and a
   motivation for "why bother with an NTT". */
static void textbook_version(const int16_t a[256], const int16_t b[256], int16_t r[256]) {
  int64_t acc[512] = {0};
  for (int i = 0; i < 256; i++)
    for (int j = 0; j < 256; j++)
      acc[i + j] += static_cast<int64_t>(a[i]) * b[j];
  for (int k = 0; k < 256; k++)
    r[k] = center_mod_q(acc[k] - acc[k + 256]);
}

/* Times `fn` over `iters` calls per batch, RUNS batches; returns ns per call. */
template <typename F>
static std::array<double, RUNS> time_batches(F &&fn, int iters) {
  std::array<double, RUNS> t{};
  for (int run = 0; run < RUNS; run++) {
    auto t0 = clk::now();
    for (int i = 0; i < iters; i++) fn();
    auto dt = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
    t[run] = dt / iters;
  }
  std::sort(t.begin(), t.end());
  return t;
}


int main() {
  static int16_t a[256], b[256], r[256], expect[256];
  volatile int64_t sink = 0;   // keeps the optimiser from deleting the work

  std::srand(42);
  for (int i = 0; i < 256; i++) {
    a[i] = center_mod_q(std::rand() % KYBER_Q);
    b[i] = center_mod_q(std::rand() % KYBER_Q);
  }

  // Verify correctness before timing. If the baseline computes something other than what
  // the kernel computes no point in timing it 
  sw_poly_mul(a, b, r);
  textbook_version(a, b, expect);
  for (int i = 0; i < 256; i++) {
    if (center_mod_q(r[i]) != expect[i]) {
      std::printf("FAIL: baseline disagrees with textbook_version at %d (got %d, want %d)\n",
                  i, center_mod_q(r[i]), expect[i]);
      return 1;
    }
  }
  std::printf("baseline verified against textbook_version product\n\n");

  auto ntt_call = [&] { sw_poly_mul(a, b, r); sink += r[0]; };
  auto sb_call  = [&] { textbook_version(a, b, r);  sink += r[0]; };

  for (int i = 0; i < ITERS; i++) ntt_call();          // warm up

  auto t = time_batches(ntt_call, ITERS);
  std::printf("NTT poly_mul   median %8.0f ns   (min %.0f, max %.0f, n=%d x %d)\n",
              t[RUNS/2], t[0], t[RUNS-1], RUNS, ITERS);

  const int sb_iters = std::max(ITERS / 50, 1);        // textbook_version is far slower
  t = time_batches(sb_call, sb_iters);
  std::printf("textbook_version     median %8.0f ns   (min %.0f, max %.0f, n=%d x %d)\n",
              t[RUNS/2], t[0], t[RUNS-1], RUNS, sb_iters);

  std::printf("\nkernel for comparison: 906 cycles = 4530 ns at 200 MHz (compute only,\n"
              "excludes dispatch and data transfer); steady-state 218 cycles = 1090 ns\n");

  (void)sink; // tell the compiler not to optimise away the work just because we don't use the result.
  return 0;
}
