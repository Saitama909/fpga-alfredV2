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

// Textbook product in Zq[X]/(X^256+1).
static void ref_poly_mul(const int16_t a[256], const int16_t b[256], int16_t r[256]) {
  int64_t acc[512] = {0};

  for (int i = 0; i < 256; i++)
    for (int j = 0; j < 256; j++)
      acc[i + j] += (int64_t)a[i] * b[j];

  for (int k = 0; k < 256; k++)
    r[k] = center_mod_q(acc[k] - acc[k + 256]);
}

static int check_slot(const int16_t *got, const int16_t *a, const int16_t *b, int slot) {
  int16_t expected[256];
  ref_poly_mul(a, b, expected);
  int errors = 0;
  for (int i = 0; i < 256; i++) {
    if (center_mod_q(got[i]) != expected[i]) {
      if (errors < 3) {
        printf("MISMATCH slot %d coeff %d: got %d (raw %d), expected %d\n",
               slot, i, center_mod_q(got[i]), got[i], expected[i]);
      }
      errors++;
    }
  }
  return errors;
}

int main() {
  srand(42);

  /* Cosim wraps the Vitis top (poly_mul_batch). Spot-check a few slots —
     full schoolbook on all 128 is too slow for the TB. */
  static int16_t a[POLY_MUL_BATCH * 256];
  static int16_t b[POLY_MUL_BATCH * 256];
  static int16_t got[POLY_MUL_BATCH * 256];

  for (int i = 0; i < POLY_MUL_BATCH * 256; i++) {
    a[i] = center_mod_q(rand() % KYBER_Q);
    b[i] = center_mod_q(rand() % KYBER_Q);
    got[i] = 0x7fff;
  }

  poly_mul_batch(a, b, got);

  const int slots[] = {0, POLY_MUL_BATCH / 2, POLY_MUL_BATCH - 1};
  int errors = 0;
  for (int s = 0; s < 3; s++) {
    int n = slots[s];
    errors += check_slot(&got[n * 256], &a[n * 256], &b[n * 256], n);
  }

  if (errors) {
    printf("FAIL: poly_mul_batch %d mismatches across spot-check slots\n", errors);
    return 1;
  }
  printf("PASS: poly_mul_batch (batch=%d, slots 0/%d/%d)\n",
         POLY_MUL_BATCH, POLY_MUL_BATCH / 2, POLY_MUL_BATCH - 1);
  return 0;
}
