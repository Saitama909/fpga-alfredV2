#include <cstdio>
#include <cstdlib>
#include <stdint.h>

#include "../src/params.h"
#include "../src/reduction.h"
#include "../src/ntt_top.h"
#include "ref_poly.h"

/* Every slot gets its own random operands. A batch top that ignored n, reused
   one buffer across iterations, or crossed operands between slots would pass a
   single-polynomial check but fail this case */
#ifndef TEST_BATCH_COUNT
#define TEST_BATCH_COUNT POLY_MUL_MAX_BATCH
#endif

int main() {
  static_assert(TEST_BATCH_COUNT >= 1 && TEST_BATCH_COUNT <= POLY_MUL_MAX_BATCH,
                "TEST_BATCH_COUNT must be within the synthesized maximum");
  const unsigned batch_count = TEST_BATCH_COUNT;
  srand(42);

  static int16_t a[POLY_MUL_MAX_BATCH * 256];
  static int16_t b[POLY_MUL_MAX_BATCH * 256];
  static int16_t got[POLY_MUL_MAX_BATCH * 256];
  static int16_t expected[256];

  for (unsigned i = 0; i < batch_count * 256; i++) {
    a[i] = center_mod_q(rand() % KYBER_Q);
    b[i] = center_mod_q(rand() % KYBER_Q);
  }

  /* Pack into the 512-bit AXI words the kernel consumes. Byte layout matches
     the int16 array exactly -- this is a reinterpretation, not a conversion,
     which is why the host needs no change. */
  static poly_word_t aw[POLY_MUL_MAX_BATCH * POLY_MUL_WORDS_PER_POLY];
  static poly_word_t bw[POLY_MUL_MAX_BATCH * POLY_MUL_WORDS_PER_POLY];
  static poly_word_t rw[POLY_MUL_MAX_BATCH * POLY_MUL_WORDS_PER_POLY];

  for (unsigned w = 0; w < batch_count * POLY_MUL_WORDS_PER_POLY; w++) {
    for (int k = 0; k < POLY_MUL_COEFFS_PER_WORD; k++) {
      int idx = w * POLY_MUL_COEFFS_PER_WORD + k;
      aw[w].range(16 * k + 15, 16 * k) = (ap_uint<16>)(uint16_t)a[idx];
      bw[w].range(16 * k + 15, 16 * k) = (ap_uint<16>)(uint16_t)b[idx];
    }
  }

  poly_mul_batch(hls::burst_maxi<poly_word_t>(aw),
                 hls::burst_maxi<poly_word_t>(bw),
                 hls::burst_maxi<poly_word_t>(rw),
                 batch_count);

  for (unsigned w = 0; w < batch_count * POLY_MUL_WORDS_PER_POLY; w++) {
    for (int k = 0; k < POLY_MUL_COEFFS_PER_WORD; k++) {
      got[w * POLY_MUL_COEFFS_PER_WORD + k] =
          (int16_t)(uint16_t)rw[w].range(16 * k + 15, 16 * k);
    }
  }

  int errors = 0, bad_slots = 0;
  for (unsigned n = 0; n < batch_count; n++) {
    ref_poly_mul(&a[n * 256], &b[n * 256], expected);

    int slot_errors = 0;
    for (int i = 0; i < 256; i++) {
      // lazily reduced output, so compare as residues mod q
      if (center_mod_q(got[n * 256 + i]) != expected[i]) {
        if (errors < 10) {
          printf("MISMATCH slot %u coeff %d: got %d (raw %d), expected %d\n",
                 n, i, center_mod_q(got[n * 256 + i]), got[n * 256 + i], expected[i]);
        }
        slot_errors++;
        errors++;
      }
    }
    if (slot_errors) bad_slots++;
  }

  if (errors == 0) {
    printf("PASS: all %u slots match the schoolbook product\n", batch_count);
    return 0;
  }
  printf("FAIL: %d coefficients across %d/%u slots mismatched\n",
         errors, bad_slots, batch_count);
  return 1;
}
