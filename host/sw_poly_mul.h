#ifndef SW_POLY_MUL_H
#define SW_POLY_MUL_H

#include <stdint.h>

/* Software reference for the poly_mul kernel, for the A53 baseline and for
   checking the hardware result. 
   
   Self-contained: the pq-crystals reference NTTis reproduced here so the host 
   builds without having to checkout kyber on the board. Same code path bench/bench_poly_mul.cpp times. 
   TODO: consolidate this this test with bench_poly_mul.cpp
*/

#define SW_KYBER_N 256
#define SW_KYBER_Q 3329

/* Centered representative of x in (-q/2, q/2]. */
int16_t sw_center_mod_q(int64_t x);

/* r = a * b in Rq, via the NTT -- what the kernel computes. */
void sw_poly_mul(const int16_t a[SW_KYBER_N], const int16_t b[SW_KYBER_N],
                 int16_t r[SW_KYBER_N]);

/* r = a * b by negacyclic convolution. Independent of the NTT, so it is the
   correctness reference: a wrong twiddle table cannot satisfy both. */
void sw_poly_mul_schoolbook(const int16_t a[SW_KYBER_N], const int16_t b[SW_KYBER_N],
                            int16_t r[SW_KYBER_N]);

#endif /* SW_POLY_MUL_H */
