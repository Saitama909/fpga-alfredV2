#ifndef NTT_TOP_H
#define NTT_TOP_H

#include <stdint.h>
#include "params.h"
#include "ap_int.h"

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

/* Forward NTT + Barrett reduce (matches ref poly_ntt). */
void hls_poly_ntt(const int16_t in[256], int16_t out[256]);

/* Kyber IND-CPA encrypt NTT-domain stage. Not KYBER_NAMESPACE-mangled. */
void kyber_enc_core(const int16_t A_T[KYBER_K][KYBER_K][256],
                    const int16_t t_hat[KYBER_K][256],
                    const int16_t r[KYBER_K][256],
                    const int16_t e1[KYBER_K][256],
                    const int16_t e2[256],
                    const int16_t msg[256],
                    int16_t u[KYBER_K][256],
                    int16_t v[256]);

/* Fused top: r = a * b in Rq. Forward NTT of both operands, pointwise
   multiply, inverse NTT. POLY_MUL_BATCH packs that many into one dispatch. */
#ifndef POLY_MUL_BATCH
#define POLY_MUL_BATCH 128
#endif

#define poly_mul KYBER_NAMESPACE(poly_mul)
void poly_mul(const int16_t a[256], const int16_t b[256], int16_t r[256]);

#define poly_mul_batch KYBER_NAMESPACE(poly_mul_batch)
void poly_mul_batch(const int16_t a[POLY_MUL_BATCH * 256],
                    const int16_t b[POLY_MUL_BATCH * 256],
                    int16_t r[POLY_MUL_BATCH * 256]);

#endif /* NTT_TOP_H */
