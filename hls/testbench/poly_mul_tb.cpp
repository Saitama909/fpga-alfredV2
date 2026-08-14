#include <cstdio>
#include <cstdlib>
#include <stdint.h>

#include "../src/params.h"
#include "../src/reduction.h"
#include "../src/ntt_top.h"
#include "ref_poly.h"

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
