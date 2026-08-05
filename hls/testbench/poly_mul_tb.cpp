#include <cstdio>
#include <cstdlib>
#include <stdint.h>

#include "../src/params.h"
#include "../src/reduction.h"
#include "../src/ntt_top.h"

// Centered representative in (-q/2, q/2]
static int16_t center_mod_q(int64_t x) {
  int64_t r = x % KYBER_Q;
  if (r < 0) r += KYBER_Q;
  if (r > KYBER_Q / 2) r -= KYBER_Q;
  return (int16_t)r;
}

// Textbook product in Zq[X]/(X^256+1). Independent of the hls NTT, so wrong twiddle table cannot satisfy both.
static void ref_poly_mul(const int16_t a[256], const int16_t b[256], int16_t r[256]) {
  int64_t acc[512] = {0};

  for (int i = 0; i < 256; i++)
    for (int j = 0; j < 256; j++)
      acc[i + j] += (int64_t)a[i] * b[j];

  // X^256 == -1, so the upper half folds back with a sign flip
  for (int k = 0; k < 256; k++)
    r[k] = center_mod_q(acc[k] - acc[k + 256]);
}

int main() {
  srand(42);

  int16_t a[256], b[256], got[256], expected[256];

  for (int i = 0; i < 256; i++) {
    a[i] = center_mod_q(rand() % KYBER_Q);
    b[i] = center_mod_q(rand() % KYBER_Q);
  }

  poly_mul(a, b, got);
  ref_poly_mul(a, b, expected);

  int errors = 0;
  for (int i = 0; i < 256; i++) {
    // poly_mul output is only lazily reduced, so compare as residues mod q
    if (center_mod_q(got[i]) != expected[i]) {
      if (errors < 10) {
        printf("MISMATCH at %d: got %d (raw %d), expected %d\n",
               i, center_mod_q(got[i]), got[i], expected[i]);
      }
      errors++;
    }
  }

  if (errors == 0) {
    printf("PASS: poly_mul matches schoolbook product for all 256 coefficients\n");
    return 0;
  }
  printf("FAIL: %d/%d coefficients mismatched\n", errors, 256);
  return 1;
}
