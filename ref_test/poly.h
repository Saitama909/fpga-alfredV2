#ifndef POLY_H
#define POLY_H

#include <stdint.h>
#include "params.h"

typedef struct {
  int16_t coeffs[KYBER_N];
} poly;

#ifdef __cplusplus
extern "C" {
#endif

void poly_compress(uint8_t r[KYBER_POLYCOMPRESSEDBYTES], const poly *a);
void poly_decompress(poly *r, const uint8_t a[KYBER_POLYCOMPRESSEDBYTES]);
void poly_tobytes(uint8_t r[KYBER_POLYBYTES], const poly *a);
void poly_frombytes(poly *r, const uint8_t a[KYBER_POLYBYTES]);
void poly_frommsg(poly *r, const uint8_t msg[KYBER_INDCPA_MSGBYTES]);
void poly_tomsg(uint8_t msg[KYBER_INDCPA_MSGBYTES], const poly *a);
void poly_getnoise_eta1(poly *r, const uint8_t seed[KYBER_SYMBYTES], uint8_t nonce);
void poly_getnoise_eta2(poly *r, const uint8_t seed[KYBER_SYMBYTES], uint8_t nonce);
void poly_ntt(poly *r);
void poly_invntt_tomont(poly *r);
void poly_basemul_montgomery(poly *r, const poly *a, const poly *b);
void poly_tomont(poly *r);
void poly_reduce(poly *r);
void poly_add(poly *r, const poly *a, const poly *b);
void poly_sub(poly *r, const poly *a, const poly *b);

#ifdef __cplusplus
}
#endif

#endif