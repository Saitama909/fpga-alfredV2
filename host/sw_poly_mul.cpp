/*
 * Software reference for poly_mul, run on the A53 as the baseline the
 * accelerator is measured against.
 *
 * This is the unmodified pq-crystals reference algorithm (ref/ntt.c,
 * ref/reduce.c), reproduced here rather than linked so the host application
 * has no dependency on a kyber checkout being present in the sysroot. It is
 * deliberately NOT the restructured HLS source in hls/src -- the baseline must
 * be the code a CPU implementer would actually write.
 */

#include <cstring>
#include "sw_poly_mul.h"

#define SW_QINV -3327 /* q^-1 mod 2^16 */


static const int16_t zetas[128] = {
  -1044,  -758,  -359, -1517,  1493,  1422,   287,   202,
   -171,   622,  1577,   182,   962, -1202, -1474,  1468,
    573, -1325,   264,   383,  -829,  1458, -1602,  -130,
   -681,  1017,   732,   608, -1542,   411,  -205, -1571,
   1223,   652,  -552,  1015, -1293,  1491,  -282, -1544,
    516,    -8,  -320,  -666, -1618, -1162,   126,  1469,
   -853,   -90,  -271,   830,   107, -1421,  -247,  -951,
   -398,   961, -1508,  -725,   448, -1065,   677, -1275,
  -1103,   430,   555,   843, -1251,   871,  1550,   105,
    422,   587,   177,  -235,  -291,  -460,  1574,  1653,
   -246,   778,  1159,  -147,  -777,  1483,  -602,  1119,
  -1590,   644,  -872,   349,   418,   329,  -156,   -75,
    817,  1097,   603,   610,  1322, -1285, -1465,   384,
  -1215,  -136,  1218, -1335,  -874,   220, -1187, -1659,
  -1185, -1530, -1278,   794, -1510,  -854,  -870,   478,
   -108,  -308,   996,   991,   958, -1460,  1522,  1628
};

/* Montgomery reduction: 16-bit result congruent to a * R^-1 mod q, R = 2^16. */
static int16_t montgomery_reduce(int32_t a) {
  int16_t t = (int16_t)a * SW_QINV;
  return (int16_t)((a - (int32_t)t * SW_KYBER_Q) >> 16);
}

/* Barrett reduction: centered representative congruent to a mod q. */
static int16_t barrett_reduce(int16_t a) {
  const int16_t v = ((1 << 26) + SW_KYBER_Q / 2) / SW_KYBER_Q;
  int16_t t = (int16_t)(((int32_t)v * a + (1 << 25)) >> 26);
  return (int16_t)(a - t * SW_KYBER_Q);
}

static int16_t fqmul(int16_t a, int16_t b) {
  return montgomery_reduce((int32_t)a * b);
}

/* In-place forward NTT, 7 layers of Cooley-Tukey butterflies. */
static void sw_ntt(int16_t r[SW_KYBER_N]) {
  unsigned int len, start, j, k = 1;

  for (len = 128; len >= 2; len >>= 1) {
    for (start = 0; start < 256; start = j + len) {
      int16_t zeta = zetas[k++];
      for (j = start; j < start + len; j++) {
        int16_t t = fqmul(zeta, r[j + len]);
        r[j + len] = (int16_t)(r[j] - t);
        r[j]       = (int16_t)(r[j] + t);
      }
    }
  }
}

/* In-place inverse NTT with the 1/128 scaling and a Montgomery factor R
   folded into the final pass (the reference's invntt_tomont). */
static void sw_invntt(int16_t r[SW_KYBER_N]) {
  unsigned int start, len, j, k = 127;
  const int16_t f = 1441; /* mont^2 / 128 */

  for (len = 2; len <= 128; len <<= 1) {
    for (start = 0; start < 256; start = j + len) {
      int16_t zeta = zetas[k--];
      for (j = start; j < start + len; j++) {
        int16_t t = r[j];
        r[j]       = barrett_reduce((int16_t)(t + r[j + len]));
        r[j + len] = (int16_t)(r[j + len] - t);
        r[j + len] = fqmul(zeta, r[j + len]);
      }
    }
  }

  for (j = 0; j < 256; j++) r[j] = fqmul(r[j], f);
}

/* One degree-2 product in Zq[X]/(X^2 - zeta). */
static void sw_basemul(int16_t r[2], const int16_t a[2], const int16_t b[2],
                       int16_t zeta) {
  r[0]  = fqmul(a[1], b[1]);
  r[0]  = fqmul(r[0], zeta);
  r[0] = (int16_t)(r[0] + fqmul(a[0], b[0]));
  r[1]  = fqmul(a[0], b[1]);
  r[1] = (int16_t)(r[1] + fqmul(a[1], b[0]));
}

/* Pointwise product of two NTT-domain polynomials. */
static void sw_poly_basemul(int16_t r[SW_KYBER_N], const int16_t a[SW_KYBER_N],
                            const int16_t b[SW_KYBER_N]) {
  for (unsigned int i = 0; i < SW_KYBER_N / 4; i++) {
    sw_basemul(&r[4 * i],     &a[4 * i],     &b[4 * i],      zetas[64 + i]);
    sw_basemul(&r[4 * i + 2], &a[4 * i + 2], &b[4 * i + 2],
               (int16_t)(-zetas[64 + i]));
  }
}

int16_t sw_center_mod_q(int64_t x) {
  int64_t r = x % SW_KYBER_Q;
  if (r < 0) r += SW_KYBER_Q;
  if (r > SW_KYBER_Q / 2) r -= SW_KYBER_Q;
  return (int16_t)r;
}

/*************************************************
* Name:        sw_poly_mul
*
* Description: r = a * b in Rq = Zq[X]/(X^256+1). basemul contributes R^-1 and
*              invntt contributes R, so the Montgomery factors cancel and r is
*              the product mod q (lazily reduced, as on hardware).
*
* Arguments:   - const int16_t a[256], b[256]: input polynomials
*              - int16_t r[256]: output polynomial
**************************************************/
void sw_poly_mul(const int16_t a[SW_KYBER_N], const int16_t b[SW_KYBER_N],
                 int16_t r[SW_KYBER_N]) {
  int16_t ta[SW_KYBER_N], tb[SW_KYBER_N];

  std::memcpy(ta, a, sizeof ta);
  std::memcpy(tb, b, sizeof tb);
  sw_ntt(ta);
  sw_ntt(tb);
  sw_poly_basemul(r, ta, tb);
  sw_invntt(r);
}

/*************************************************
* Name:        sw_poly_mul_schoolbook
*
* Description: r = a * b by direct negacyclic convolution: X^256 == -1, so the
*              upper half of the product folds back with a sign flip. O(n^2)
*              and far slower, but completely independent of the NTT, so it is what the
*              hardware result is checked against.
*
* Arguments:   - const int16_t a[256], b[256]: input polynomials
*              - int16_t r[256]: output polynomial, centered mod q
**************************************************/
void sw_poly_mul_schoolbook(const int16_t a[SW_KYBER_N], const int16_t b[SW_KYBER_N],
                            int16_t r[SW_KYBER_N]) {
  int64_t acc[2 * SW_KYBER_N] = {0};

  for (int i = 0; i < SW_KYBER_N; i++)
    for (int j = 0; j < SW_KYBER_N; j++)
      acc[i + j] += (int64_t)a[i] * b[j];

  for (int k = 0; k < SW_KYBER_N; k++)
    r[k] = sw_center_mod_q(acc[k] - acc[k + SW_KYBER_N]);
}
