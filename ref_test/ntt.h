#ifndef NTT_TOP_H
#define NTT_TOP_H

#include <stdint.h>
#include "params.h"

#ifdef __cplusplus
extern "C" {
#endif

#define zetas KYBER_NAMESPACE(zetas)
extern const int16_t zetas[128];

#define zetas_inv KYBER_NAMESPACE(zetas_inv)
extern const int16_t zetas_inv[128];

#define ntt KYBER_NAMESPACE(ntt)
void ntt(int16_t poly[256]);

#define invntt KYBER_NAMESPACE(invntt)
void invntt(int16_t poly[256]);

#define invntt_tomont KYBER_NAMESPACE(invntt_tomont)
void invntt_tomont(int16_t poly[256]);

#define basemul KYBER_NAMESPACE(basemul)
void basemul(int16_t r[2], const int16_t a[2], const int16_t b[2], int16_t zeta);

void hls_poly_basemul(int16_t r[256], const int16_t a[256], const int16_t b[256]);
void hls_poly_add(int16_t r[256], const int16_t a[256], const int16_t b[256]);
void hls_poly_sub(int16_t r[256], const int16_t a[256], const int16_t b[256]);
void hls_poly_reduce(int16_t r[256]);
void hls_poly_tomont(int16_t r[256]);

void hls_polyvec_basemul_acc(int16_t r[256],
                             const int16_t a[KYBER_K][256],
                             const int16_t b[KYBER_K][256]);

void hls_matvec_ntt(int16_t out[KYBER_K+1][256],
                    const int16_t A[KYBER_K+1][KYBER_K][256],
                    const int16_t s_hat[KYBER_K][256]);

#ifdef __cplusplus
}
#endif

#endif /* NTT_TOP_H */
