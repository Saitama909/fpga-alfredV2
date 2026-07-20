#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "../src/params.h"
#include "../src/reduction.h"
#include "../src/ntt_top.h"

// Centered representative in (-q/2, q/2]
static int16_t center_mod_q(int32_t x) {
  int32_t r = x % KYBER_Q;
  if (r < 0) r += KYBER_Q;
  if (r > KYBER_Q / 2) r -= KYBER_Q;
  return (int16_t)r;
}

int main() {
  srand(42);

  int16_t orig[256];
  int16_t work[256];

  for (int i = 0; i < 256; i++) {
    orig[i] = i-128;  // center_mod_q(rand() % KYBER_Q);
    work[i] = orig[i]+1;
  }

  ntt(work);
  invntt(work);

  // invntt(ntt(r)) == r * MONT (mod q), per the ntt_top.cpp doc comment:
  // invntt performs the inverse transform AND multiplies by the
  // Montgomery factor 2^16 mod q.
  // Outputs are only "lazily" reduced (range (-q, q), not fully centered),
  // so compare as residues mod q rather than requiring exact equality.
  int errors = 0;
  for (int i = 0; i < 256; i++) {
    int16_t expected = center_mod_q((int32_t)orig[i] * MONT);
    int16_t got = center_mod_q(work[i]);
    if (got != expected) {
      if (errors < 10) {
        printf("MISMATCH at %d: got %d (raw %d), expected %d (orig %d)\n",
               i, got, work[i], expected, orig[i]);
      }
      errors++;
    }
  }

  if (errors == 0) {
    printf("PASS: invntt(ntt(r)) == r * MONT (mod q) for all 256 coefficients\n");
    return 0;
  } else {
    printf("FAIL: %d/%d coefficients mismatched\n", errors, 256);
    return 1;
  }
}
