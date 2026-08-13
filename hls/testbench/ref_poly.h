#ifndef REF_POLY_H
#define REF_POLY_H

/* Shared reference arithmetic for the HLS testbenches.
 *
 * Deliberately independent of hls/src: a schoolbook convolution and a
 * straightforward modular reduction, so a wrong twiddle table, a wrong
 * Montgomery constant, or a broken butterfly cannot satisfy both this and the
 * kernel. Header-only, so hls_config.cfg keeps its single tb.file entry.
 */

#include <stdint.h>

#include "../src/params.h"

// Centered representative in (-q/2, q/2]
static inline int16_t center_mod_q(int64_t x) {
  int64_t r = x % KYBER_Q;
  if (r < 0) r += KYBER_Q;
  if (r > KYBER_Q / 2) r -= KYBER_Q;
  return (int16_t)r;
}

// Textbook product in Zq[X]/(X^256+1).
static inline void ref_poly_mul(const int16_t a[256], const int16_t b[256],
                                int16_t r[256]) {
  int64_t acc[512] = {0};

  for (int i = 0; i < 256; i++)
    for (int j = 0; j < 256; j++)
      acc[i + j] += (int64_t)a[i] * b[j];

  // X^256 == -1, so the upper half folds back with a sign flip
  for (int k = 0; k < 256; k++)
    r[k] = center_mod_q(acc[k] - acc[k + 256]);
}

#endif /* REF_POLY_H */
