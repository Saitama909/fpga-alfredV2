#include <stdint.h>
#include "params.h"
#include "reduction.h"

/*************************************************
* Name:        montgomery_reduce
*
* Description: Montgomery reduction; given a 32-bit integer a, computes
*              16-bit integer congruent to a * R^-1 mod q, where R=2^16
*
* Arguments:   - int32_t a: input integer to be reduced;
*                           has to be in {-q2^15,...,q2^15-1}
*
* Returns:     integer in {-q+1,...,q-1} congruent to a * R^-1 modulo q.
**************************************************/
int16_t montgomery_reduce(int32_t a)
{
  #pragma HLS INLINE
  int16_t t;

  // t = (int16_t)a * QINV, where QINV = -3327 = -(2^11 + 2^10 + 2^8 - 1).
  // Only the low 16 bits matter, so this is three 16-bit adds in fabric rather
  // than a DSP. 
  // had to writ this explicitly longhand because BIND_OP impl=fabric does not strength
  // reduce a constant multiply for some reason, it just creates a generic array multiplier 
  // (measured at 273 LUT each). The *KYBER_Q below and the zeta*b in fqmul stay on DSP.
  // freeing one multiply per fqmul is what fits 32 lanes, freeing more overshoots LUT limit 
  uint16_t x = (uint16_t)a;
  t = (int16_t)(uint16_t)(x - (x << 11) - (x << 10) - (x << 8));

  t = (a - (int32_t)t*KYBER_Q) >> 16;
  return t;
}

/*************************************************
* Name:        barrett_reduce
*
* Description: Barrett reduction; given a 16-bit integer a, computes
*              centered representative congruent to a mod q in {-(q-1)/2,...,(q-1)/2}
*
* Arguments:   - int16_t a: input integer to be reduced
*
* Returns:     integer in {-(q-1)/2,...,(q-1)/2} congruent to a modulo q.
**************************************************/
int16_t barrett_reduce(int16_t a) {
  #pragma HLS INLINE
  int16_t t;
  const int16_t v = ((1<<26) + KYBER_Q/2)/KYBER_Q;

  t  = ((int32_t)v*a + (1<<25)) >> 26;
  t *= KYBER_Q;
  return a - t;
}
