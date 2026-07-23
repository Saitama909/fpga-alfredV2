#include <stdint.h>
#include "params.h"
#include "reduction.h"

/*
 * Montgomery reduction: return something congruent to a * R^{-1} mod q,
 * with R = 2^16. Same maths as reference Kyber; we just spell the steps
 * out a bit so HLS can inline this into fqmul and park the multiplies on
 * DSPs.
 *
 * Input a must sit in {-q*2^15, ..., q*2^15 - 1}.
 * Output is in {-q+1, ..., q-1}.
 */
int16_t montgomery_reduce(int32_t a) {
  #pragma HLS INLINE

  /* Low 16 bits of a, times QINV. */
  const int16_t a_lo = (int16_t)a;
  int32_t m32 = (int32_t)a_lo * (int32_t)QINV;
  #pragma HLS BIND_OP variable=m32 op=mul impl=dsp
  const int16_t m = (int16_t)m32;

  int32_t t = (int32_t)m * (int32_t)KYBER_Q;
  #pragma HLS BIND_OP variable=t op=mul impl=dsp

  return (int16_t)((a - t) >> 16);
}

/*
 * Barrett reduction to a centred residue mod q.
 * Used on the inverse NTT path. Same idea as above: inline + DSP mul so
 * it doesn't sit as a heavy call boundary.
 *
 * Output is in {-(q-1)/2, ..., (q-1)/2}.
 */
int16_t barrett_reduce(int16_t a) {
  #pragma HLS INLINE

  const int16_t v = ((1 << 26) + KYBER_Q / 2) / KYBER_Q;

  int32_t t32 = ((int32_t)v * (int32_t)a + (1 << 25)) >> 26;
  #pragma HLS BIND_OP variable=t32 op=mul impl=dsp
  int16_t t = (int16_t)t32;
  t = (int16_t)(t * KYBER_Q);

  return (int16_t)(a - t);
}
