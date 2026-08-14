#ifndef NTT_TOP_H
#define NTT_TOP_H

#include <stdint.h>
#include "params.h"
#include "ap_int.h"
#include "hls_burst_maxi.h"
#include "hls_stream.h"

#define zetas KYBER_NAMESPACE(zetas)
extern const ap_int<12> zetas[128];

#define zetas_inv KYBER_NAMESPACE(zetas_inv)
extern const ap_int<12> zetas_inv[128];

#define ntt KYBER_NAMESPACE(ntt)
void ntt(const int16_t in[256], int16_t out[256]); // made out-of-place to enable dataflow

#define invntt KYBER_NAMESPACE(invntt)
void invntt(const int16_t in[256], int16_t out[256]);

void basemul(int16_t a0, int16_t a1, int16_t b0, int16_t b1,
             int16_t zeta, int16_t &r0, int16_t &r1);

void hls_poly_basemul(int16_t r[256], const int16_t a[256], const int16_t b[256]);
void hls_poly_add(int16_t r[256], const int16_t a[256], const int16_t b[256]);
void hls_poly_reduce(int16_t r[256]);

void hls_polyvec_basemul_acc(int16_t r[256],
                             const int16_t a[KYBER_K][256],
                             const int16_t b[KYBER_K][256]);

void hls_matvec_ntt(int16_t out[KYBER_K+1][256],
                    const int16_t A[KYBER_K+1][KYBER_K][256],
                    const int16_t s_hat[KYBER_K][256]);
void hls_poly_tomont(int16_t r[256]);
void hls_poly_sub(int16_t r[256], const int16_t a[256], const int16_t b[256]);

/* Fused top: r = a * b in Rq. Forward NTT of both operands, pointwise
   multiply, inverse NTT. */
#define poly_mul KYBER_NAMESPACE(poly_mul)
void poly_mul(const int16_t a[256], const int16_t b[256], int16_t r[256]);

/* Maximum polynomials per kernel dispatch. The host supplies the actual count
   at runtime, so partial batches do not require a new xclbin. Keeping a
   compile-time maximum gives HLS bounded loops and protects the AXI accesses. */
#ifndef POLY_MUL_MAX_BATCH
#define POLY_MUL_MAX_BATCH 256
#endif
/* AXI word carrying the operands. The width is the whole point: burst_maxi
   takes its port width from sizeof(T)*8, and an int16 port cannot be widened
   under the batch top -- csynth reports
     "Could not widen since type i16 size is >= alignment 2(bytes)"
   because a derived pointer &a[n*256] loses the port's declared 64-byte
   alignment. At 512 bits the element is itself 64 bytes, so there is nothing
   left to widen and nothing to prove. 8 words per polynomial is also the AXI
   floor: 256 x int16 = 512 B = 8 beats. */
#define POLY_MUL_WIDE_BITS       512
#define POLY_MUL_COEFFS_PER_WORD (POLY_MUL_WIDE_BITS / 16)          /* 32 */
#define POLY_MUL_WORDS_PER_POLY  (256 / POLY_MUL_COEFFS_PER_WORD)   /*  8 */

typedef ap_uint<POLY_MUL_WIDE_BITS> poly_word_t;

/* Batched top: batch_count independent products in one dispatch. Operands are
   packed back to back, polynomial n occupying words [n*8, n*8+8). Byte layout
   is identical to the int16 packing, so the host needs no repacking. */
#define poly_mul_batch KYBER_NAMESPACE(poly_mul_batch)
void poly_mul_batch(hls::burst_maxi<poly_word_t> a,
                    hls::burst_maxi<poly_word_t> b,
                    hls::burst_maxi<poly_word_t> r,
                    unsigned batch_count);

#endif /* NTT_TOP_H */
