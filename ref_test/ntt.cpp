// ==========================================================================
  // ==========================================================================
  // ntt.cpp -- Kyber NTT-domain arithmetic, HLS implementation.
  //
  // Drop-in replacement for the pq-crystals reference ntt.c, validated
  // byte-exact against the published test vectors (SHA256SUMS) for K=2/3/4.
  //
  // Structural changes from the reference:
  //   - ntt/invntt built as 7 templated stages writing to separate buffers
  //     instead of one in-place loop nest, so HLS DATAFLOW can pipeline them.
  //     Twiddle index is the closed form zetas[128/LEN + g] rather than a
  //     running counter, removing the loop-carried dependency.
  //   - invntt uses a second table, zetas_inv, which is pre-negated and
  //     pre-reversed; hence the (t - b) subtraction where the reference has
  //     (b - t). The two sign flips cancel -- see the note above the table.
  //   - The 1/128 scaling is fused into the final inverse stage (SCALE
  //     template flag) instead of a separate 256-coefficient pass.
  //
  // Changes made for the software build, neither affecting behaviour:
  //   - zetas/zetas_inv are int16_t rather than ap_int<12>, so the reference
  //     poly.c (compiled as C) can read the table directly. All values fit in
  //     12 bits either way; cost is 64 bytes of ROM the synthesiser would
  //     otherwise have saved.
  //   - invntt_tomont added as an alias for invntt -- the name poly.c calls.
  //
  // Also provides an hls_* polynomial/vector/matrix layer. When linking
  // against the reference these are reached through shims at the bottom of
  // this file, with the corresponding bodies commented out in poly.c and
  // polyvec.c. hls_matvec_ntt has no reference counterpart: its (K+1) x K
  // stacking of A^T over t^T is a hardware-motivated unification of the two
  // matrix products in encryption.
  // ==========================================================================

  #include <stdint.h>
  #include <cstring>
  #include "params.h"
  #include "ntt.h"
  #include "reduce.h"
  #include "poly.h"
  #include "polyvec.h"

  /* Code to generate zetas and zetas_inv used in the number-theoretic transform:

  #define KYBER_ROOT_OF_UNITY 17

  static const uint8_t tree[128] = {
    0, 64, 32, 96, 16, 80, 48, 112, 8, 72, 40, 104, 24, 88, 56, 120,
    4, 68, 36, 100, 20, 84, 52, 116, 12, 76, 44, 108, 28, 92, 60, 124,
    2, 66, 34, 98, 18, 82, 50, 114, 10, 74, 42, 106, 26, 90, 58, 122,
    6, 70, 38, 102, 22, 86, 54, 118, 14, 78, 46, 110, 30, 94, 62, 126,
    1, 65, 33, 97, 17, 81, 49, 113, 9, 73, 41, 105, 25, 89, 57, 121,
    5, 69, 37, 101, 21, 85, 53, 117, 13, 77, 45, 109, 29, 93, 61, 125,
    3, 67, 35, 99, 19, 83, 51, 115, 11, 75, 43, 107, 27, 91, 59, 123,
    7, 71, 39, 103, 23, 87, 55, 119, 15, 79, 47, 111, 31, 95, 63, 127
  };

  void init_ntt() {
    unsigned int i;
    int16_t tmp[128];

    tmp[0] = MONT;
    for(i=1;i<128;i++)
      tmp[i] = fqmul(tmp[i-1],MONT*KYBER_ROOT_OF_UNITY % KYBER_Q);

    for(i=0;i<128;i++) {
      zetas[i] = tmp[tree[i]];
      if(zetas[i] > KYBER_Q/2)
        zetas[i] -= KYBER_Q;
      if(zetas[i] < -KYBER_Q/2)
        zetas[i] += KYBER_Q;
    }
  }
  */

  const int16_t zetas[128] = {
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

  const int16_t zetas_inv[128] = {
       0,    758,   1517,    359,   -202,   -287,  -1422,  -1493,
   -1468,   1474,   1202,   -962,   -182,  -1577,   -622,    171,
    1571,    205,   -411,   1542,   -608,   -732,  -1017,    681,
     130,   1602,  -1458,    829,   -383,   -264,   1325,   -573,
    1275,   -677,   1065,   -448,    725,   1508,   -961,    398,
     951,    247,   1421,   -107,   -830,    271,     90,    853,
   -1469,   -126,   1162,   1618,    666,    320,      8,   -516,
    1544,    282,  -1491,   1293,  -1015,    552,   -652,  -1223,
   -1628,  -1522,   1460,   -958,   -991,   -996,    308,    108,
    -478,    870,    854,   1510,   -794,   1278,   1530,   1185,
    1659,   1187,   -220,    874,   1335,  -1218,    136,   1215,
    -384,   1465,   1285,  -1322,   -610,   -603,  -1097,   -817,
      75,    156,   -329,   -418,   -349,    872,   -644,   1590,
   -1119,    602,  -1483,    777,    147,  -1159,   -778,    246,
   -1653,  -1574,    460,    291,    235,   -177,   -587,   -422,
    -105,  -1550,   -871,   1251,   -843,   -555,   -430,   1103,
  };

  void poly_basemul_montgomery(poly *r, const poly *a, const poly *b) {
    hls_poly_basemul(r->coeffs, a->coeffs, b->coeffs);
  }
  void poly_add(poly *r, const poly *a, const poly *b) {
    hls_poly_add(r->coeffs, a->coeffs, b->coeffs);
  }
  void poly_sub(poly *r, const poly *a, const poly *b) {
    hls_poly_sub(r->coeffs, a->coeffs, b->coeffs);
  }
  void poly_reduce(poly *r)  { hls_poly_reduce(r->coeffs); }
  void poly_tomont(poly *r)  { hls_poly_tomont(r->coeffs); }
  /*************************************************
  * Name:        fqmul
  *
  * Description: Multiplication followed by Montgomery reduction
  *
  * Arguments:   - int16_t a: first factor
  *              - int16_t b: second factor
  *
  * Returns 16-bit integer congruent to a*b*R^{-1} mod q
  **************************************************/
  static int16_t fqmul(int16_t a, int16_t b) {
    #pragma HLS INLINE
    return montgomery_reduce((int32_t)a*b);
  }


  template <int LEN>
  static void ntt_stage(const int16_t in[256], int16_t out[256]) {
    #pragma HLS INLINE off
    for (int i = 0; i < 128; i++) {
      #pragma HLS PIPELINE II=1
      int g     = i / LEN;
      int off   = i % LEN;
      int start = g * (LEN << 1);
      int j     = start + off;
      int16_t zeta = zetas[128 / LEN + g];

      // separate in[] and out[] arrays: the reference reads and writes the
      // same array in each butterfly, which serialises the accesses in HLS
      // and needs a DEPENDENCE pragma.
      int16_t a = in[j];
      int16_t b = in[j + LEN];
      int16_t t = fqmul(zeta, b);
      out[j]       = a + t;
      out[j + LEN] = a - t;
    }
  }


  /*************************************************
  * Name:        ntt
  *
  * Description: Inplace number-theoretic transform (NTT) in Rq.
  *              input is in standard order, output is in bitreversed order
  *
  * Arguments:   - int16_t r[256]: pointer to input/output vector of elements of Zq
  **************************************************/
  void ntt(int16_t r[256]) {
    #pragma HLS DATAFLOW

    int16_t buf1[256], buf2[256], buf3[256];
    int16_t buf4[256], buf5[256], buf6[256];

    #pragma HLS ARRAY_PARTITION variable=buf1 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf2 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf3 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf4 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf5 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf6 cyclic factor=8 dim=1

    ntt_stage<128>(r,    buf1);   // reads r directly, no copy_in
    ntt_stage<64> (buf1, buf2);
    ntt_stage<32> (buf2, buf3);
    ntt_stage<16> (buf3, buf4);
    ntt_stage<8>  (buf4, buf5);
    ntt_stage<4>  (buf5, buf6);
    ntt_stage<2>  (buf6, r);      // writes r directly, no copy_out
  }

  template <int LEN, bool SCALE>
  static void invntt_stage(const int16_t in[256], int16_t out[256]) {
    #pragma HLS INLINE off
    const int16_t f = 1441;                 // mont^2 / 128
    for (int i = 0; i < 128; i++) {
      #pragma HLS PIPELINE II=1
      int g     = i / LEN;
      int off   = i % LEN;
      int start = g * (LEN << 1);
      int j     = start + off;
      int16_t zeta = zetas_inv[128 / LEN + g];
      int16_t t = in[j];
      int16_t b = in[j + LEN];
      int16_t s = barrett_reduce(t + b);
      int16_t d = fqmul(zeta, (int16_t)(t - b));
      // the inverse transform needs a final multiply-by-1/128 on every
      // coefficient; fusing it into the last stage saves a separate pass
      if (SCALE) {
        s = fqmul(s, f);
        d = fqmul(d, f);
      }
      out[j]       = s;
      out[j + LEN] = d;
    }
  }

  /*************************************************
  * Name:        invntt
  *
  * Description: Inplace inverse number-theoretic transform in Rq and
  *              multiplication by Montgomery factor 2^16.
  *              Input is in bitreversed order, output is in standard order
  *
  *              The extra factor of R is not a quirk -- it cancels the R^-1
  *              that basemul leaves behind.
  *
  * Arguments:   - int16_t r[256]: pointer to input/output vector of elements of Zq
  **************************************************/
  void invntt(int16_t r[256]) {
    #pragma HLS DATAFLOW

    int16_t buf1[256], buf2[256], buf3[256];
    int16_t buf4[256], buf5[256], buf6[256];
    #pragma HLS ARRAY_PARTITION variable=buf1 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf2 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf3 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf4 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf5 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf6 cyclic factor=8 dim=1

    invntt_stage<2,   false>(r,    buf1);   // reads r directly, no copy_in
    invntt_stage<4,   false>(buf1, buf2);
    invntt_stage<8,   false>(buf2, buf3);
    invntt_stage<16,  false>(buf3, buf4);
    invntt_stage<32,  false>(buf4, buf5);
    invntt_stage<64,  false>(buf5, buf6);
    invntt_stage<128, true> (buf6, r);      // writes r directly + applies 1/128
  }

  /*************************************************
  * Name:        invntt_tomont
  *
  * Description: Name the reference poly.c calls. Identical to invntt.
  *              The reference applies the 1/128 scaling as a separate pass
  *              over all 256 coefficients; invntt fuses it into the final
  *              stage instead, which covers both halves of every butterfly
  *              and so reaches all 256. Same result, one fewer pass.
  **************************************************/
  void invntt_tomont(int16_t r[256]) {
    invntt(r);
  }

  /*************************************************
  * Name:        basemul
  *
  * Description: Multiplication of polynomials in Zq[X]/(X^2-zeta)
  *              used for multiplication of elements in Rq in NTT domain
  *
  * Arguments:   - int16_t r[2]: pointer to the output polynomial
  *              - const int16_t a[2]: pointer to the first factor
  *              - const int16_t b[2]: pointer to the second factor
  *              - int16_t zeta: integer defining the reduction polynomial
  **************************************************/
  void basemul(int16_t r[2], const int16_t a[2], const int16_t b[2], int16_t zeta)
  {
    #pragma HLS INLINE
    r[0]  = fqmul(a[1], b[1]);
    r[0]  = fqmul(r[0], zeta);
    r[0] += fqmul(a[0], b[0]);
    r[1]  = fqmul(a[0], b[1]);
    r[1] += fqmul(a[1], b[0]);
  }

  // ==========================================================================
  // Polynomial / vector / matrix layer.
  //
  // Not used when linking against the reference (its poly.c and polyvec.c
  // provide equivalents), but kept for the HLS build and the standalone
  // testbench. The hls_ prefix keeps them clear of the reference's
  // KYBER_NAMESPACE-mangled poly_* symbols.
  // ==========================================================================

  void hls_poly_basemul(int16_t r[256], const int16_t a[256], const int16_t b[256]) {
    for (int i = 0; i < 64; i++) {
      int16_t z = zetas[64+i];
      basemul(&r[4*i],   &a[4*i],   &b[4*i],    z);
      basemul(&r[4*i+2], &a[4*i+2], &b[4*i+2], -z);
    }
  }

  void hls_poly_add(int16_t r[256], const int16_t a[256], const int16_t b[256]) {
    for (int i = 0; i < 256; i++) r[i] = a[i] + b[i];
  }

  void hls_poly_reduce(int16_t r[256]) {
    for (int i = 0; i < 256; i++) r[i] = barrett_reduce(r[i]);
  }

  void hls_polyvec_basemul_acc(int16_t r[256],
                               const int16_t a[KYBER_K][256],
                               const int16_t b[KYBER_K][256]) {
    int16_t t[256];
    hls_poly_basemul(r, a[0], b[0]);
    for (int i = 1; i < KYBER_K; i++) {
      hls_poly_basemul(t, a[i], b[i]);
      hls_poly_add(r, r, t);
    }
    hls_poly_reduce(r);
  }

  void hls_matvec_ntt(int16_t out[KYBER_K+1][256],
                      const int16_t A[KYBER_K+1][KYBER_K][256],
                      const int16_t s_hat[KYBER_K][256]) {
    for (int i = 0; i < KYBER_K+1; i++)
      hls_polyvec_basemul_acc(out[i], A[i], s_hat);
  }

  /*************************************************
  * Name:        hls_poly_tomont
  *
  * Description: Converts a polynomial to Montgomery domain, i.e. multiplies
  *              every coefficient by R = 2^16 mod q.
  *
  *              Needed ONLY where a base multiply is not followed by an
  *              inverse NTT. invntt normally supplies that R; key generation
  *              never calls invntt, so this pays the debt by hand.
  *
  *              The constant is R^2, not R, because montgomery_reduce always
  *              divides by R: r * R^2 * R^-1 = r * R.
  **************************************************/
  void hls_poly_tomont(int16_t r[256]) {
    const int16_t f = 1353;   // R^2 mod q, where R = 2^16
    for (int i = 0; i < 256; i++)
      r[i] = montgomery_reduce((int32_t)r[i] * f);
  }

  /*************************************************
  * Name:        hls_poly_sub
  *
  * Description: r = a - b, coefficient-wise. No reduction -- caller reduces.
  *              Used in decryption: v - InvNTT(s_hat o u_hat).
  **************************************************/
  void hls_poly_sub(int16_t r[256], const int16_t a[256], const int16_t b[256]) {
    for (int i = 0; i < 256; i++)
      r[i] = a[i] - b[i];
  }

  void polyvec_basemul_acc_montgomery(poly *r, const polyvec *a, const polyvec *b) {
    int16_t A[KYBER_K][256], B[KYBER_K][256];
    for (int i = 0; i < KYBER_K; i++) {
      memcpy(A[i], a->vec[i].coeffs, 512);
      memcpy(B[i], b->vec[i].coeffs, 512);
    }
    hls_polyvec_basemul_acc(r->coeffs, A, B);
  }