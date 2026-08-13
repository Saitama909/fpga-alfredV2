  #include <stdint.h>
  #include "params.h"
  #include "ntt_top.h"
  #include "reduction.h"
  #include "ap_int.h"

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

  const ap_int<12> zetas[128] = {
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

  const ap_int<12> zetas_inv[128] = {
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
      // builds 4 butterflies working in parallel
      #pragma HLS UNROLL factor=4 
      // loop flatten
      int g     = i / LEN;
      int off   = i % LEN;
      int start = g * (LEN << 1);
      int j     = start + off;
      int16_t zeta = zetas[128 / LEN + g];

      // original read and wrote the same array in each butterfly, which forces the hardware to serialise those accesses 
      // (and needed a DEPENDENCE pragma).
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
  * Description: Number-theoretic transform (NTT) in Rq. Out-of-place although the
  *              reference is in-place. writing each of the 7 stages into
  *              its own buffer lets DATAFLOW pipeline them.
  *              Input is in standard order, output is in bitreversed order.
  *
  * Arguments:   - const int16_t in[256]:  input vector of elements of Zq
  *              - int16_t out[256]:       output vector, distinct from in
  **************************************************/

  void ntt(const int16_t in[256], int16_t out[256]) {
    // Keep as a sub-block so poly_mul can reuse it. inlining it would merge its DATAFLOW region into the caller.
    #pragma HLS INLINE off
    #pragma HLS DATAFLOW

    int16_t buf1[256], buf2[256], buf3[256];
    int16_t buf4[256], buf5[256], buf6[256];
    
    #pragma HLS ARRAY_PARTITION variable=buf1 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf2 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf3 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf4 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf5 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf6 cyclic factor=8 dim=1
    // dataflow style
    ntt_stage<128>(in,   buf1);
    ntt_stage<64> (buf1, buf2);
    ntt_stage<32> (buf2, buf3);
    ntt_stage<16> (buf3, buf4);
    ntt_stage<8>  (buf4, buf5);
    ntt_stage<4>  (buf5, buf6);
    ntt_stage<2>  (buf6, out);
  }

  template <int LEN, bool SCALE>
  static void invntt_stage(const int16_t in[256], int16_t out[256]) {
    #pragma HLS INLINE off
    const int16_t f = 1441;                 // mont^2 / 128  (512 for plain 1/128)
    for (int i = 0; i < 128; i++) {
      #pragma HLS PIPELINE II=1
      #pragma HLS UNROLL factor=4
      int g     = i / LEN;
      int off   = i % LEN;
      int start = g * (LEN << 1);
      int j     = start + off;
      int16_t zeta = zetas_inv[128 / LEN + g];
      int16_t t = in[j];
      int16_t b = in[j + LEN];
      int16_t s = barrett_reduce(t + b);
      int16_t d = fqmul(zeta, (int16_t)(t - b));
      // inverse NTT needs a final multiply-by-1/128 on every coefficient
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
  * Description: Inverse number-theoretic transform in Rq, with multiplication
  *              by the Montgomery factor 2^16. Out-of-place for the same
  *              reason as ntt. Input is in bitreversed order, output is in
  *              standard order.
  *
  * Arguments:   - const int16_t in[256]:  input vector of elements of Zq
  *              - int16_t out[256]:       output vector, distinct from in
  **************************************************/
  void invntt(const int16_t in[256], int16_t out[256]) {
    #pragma HLS INLINE off
    #pragma HLS DATAFLOW

    int16_t buf1[256], buf2[256], buf3[256];
    int16_t buf4[256], buf5[256], buf6[256];
    #pragma HLS ARRAY_PARTITION variable=buf1 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf2 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf3 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf4 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf5 cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=buf6 cyclic factor=8 dim=1

    invntt_stage<2,   false>(in,   buf1);
    invntt_stage<4,   false>(buf1, buf2);
    invntt_stage<8,   false>(buf2, buf3);
    invntt_stage<16,  false>(buf3, buf4);
    invntt_stage<32,  false>(buf4, buf5);
    invntt_stage<64,  false>(buf5, buf6);
    invntt_stage<128, true> (buf6, out);    // applies the 1/128 scaling
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
  void basemul(int16_t a0, int16_t a1,
                             int16_t b0, int16_t b1,
                             int16_t zeta,
                             int16_t &r0, int16_t &r1)
  {
    #pragma HLS INLINE
    int16_t t = fqmul(a1, b1);
    r0 = fqmul(t, zeta) + fqmul(a0, b0);
    r1 = fqmul(a0, b1)  + fqmul(a1, b0);
}


  // Pointwise multiply in the NTT domain; result carries a factor of R^-1.
  // INLINE off only so it shows as its own module in the synthesis report.
  void hls_poly_basemul(int16_t r[256], const int16_t a[256], const int16_t b[256]) {
    #pragma HLS INLINE off
    for (int i = 0; i < 64; i++) {
      // unroll 2 touches indices 8i..8i+7 per cycle: one element per bank, given
      // the caller's buffers are cyclic-8 partitioned
      #pragma HLS PIPELINE II=1
      #pragma HLS UNROLL factor=2
      int16_t z = (int16_t)zetas[64+i];
      basemul(a[4*i],   a[4*i+1], b[4*i],   b[4*i+1],  z, r[4*i],   r[4*i+1]);
      basemul(a[4*i+2], a[4*i+3], b[4*i+2], b[4*i+3], -z, r[4*i+2], r[4*i+3]);
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
  *              inverse NTT. Normally inverse NTT will contain a multiple by R
  *              due to fqmul, however in key generation this is not the case
  **************************************************/
  void hls_poly_tomont(int16_t r[256]) {
    const int16_t f = 1353;   // R^2 mod q, where R = 2^16
    for (int i = 0; i < 256; i++)
      r[i] = montgomery_reduce((int32_t)r[i] * f);
  }

  /*************************************************
  * Name:        hls_poly_sub
  *
  * Description: r = a - b, coefficient-wise
  *              Used in decryption, doesn't auto reduce
  **************************************************/
  void hls_poly_sub(int16_t r[256], const int16_t a[256], const int16_t b[256]) {
    for (int i = 0; i < 256; i++)
      r[i] = a[i] - b[i];
  }

  /*************************************************
  * Name:        poly_mul
  *
  * Description: r = a * b in Rq = Zq[X]/(X^256+1), via the NTT. Forward
  *              transform both operands, multiply pointwise, transform back.
  *              basemul contributes R^-1 and invntt contributes R, so the
  *              Montgomery factors cancel and r is the plain product mod q
  *              (lazily reduced: congruent mod q, not necessarily centered).
  *
  * Arguments:   - const int16_t a[256], b[256]: input polynomials
  *              - int16_t r[256]: output polynomial
  **************************************************/
  void poly_mul(const int16_t a[256], const int16_t b[256], int16_t r[256]) {
    // inlining this is a structural choice poly_mul_batch wraps this call in its own
    // DATAFLOW region, and inlining here would merge these processes into the
    // caller's and destroy both structures.
    #pragma HLS INLINE off
    #pragma HLS DATAFLOW
    
    int16_t na[256], nb[256], tr[256];
    #pragma HLS ARRAY_PARTITION variable=na cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=nb cyclic factor=8 dim=1
    #pragma HLS ARRAY_PARTITION variable=tr cyclic factor=8 dim=1

    // separate processes under DATAFLOW, so these should run concurrently as two
    // instances rather than sharing one
    ntt(a, na);
    ntt(b, nb);

    hls_poly_basemul(tr, na, nb);
    invntt(tr, r);
  }

  /* Whole-batch streaming interface.
    notes

     One request per port per dispatch. it splits it into AXI
     bursts of max_read_burst_length internally and keeps up to 16 in flight from my undestanding.
     so the round trip is paid once rather than once per polynomial. 
  */
  static void read_all(hls::burst_maxi<poly_word_t>& m,
                       hls::stream<poly_word_t>& s,
                       unsigned batch_count) {
    #pragma HLS INLINE off

    const unsigned total = batch_count * POLY_MUL_WORDS_PER_POLY;

    m.read_request(0, total);

    read_all_loop:
    for (unsigned w = 0; w < total; ++w) {
      #pragma HLS PIPELINE II=1
      #pragma HLS LOOP_TRIPCOUNT \
          min=POLY_MUL_WORDS_PER_POLY \
          max=POLY_MUL_MAX_BATCH*POLY_MUL_WORDS_PER_POLY

      s.write(m.read());
    }
  }


  static void write_all(hls::burst_maxi<poly_word_t>& m,
                        hls::stream<poly_word_t>& s,
                        unsigned batch_count) {
    #pragma HLS INLINE off
    const unsigned total = batch_count * POLY_MUL_WORDS_PER_POLY;
    m.write_request(0, total);
    write_all_loop:
    for (unsigned w = 0; w < total; ++w) {
      #pragma HLS PIPELINE II=1
      #pragma HLS LOOP_TRIPCOUNT \
          min=POLY_MUL_WORDS_PER_POLY \
          max=POLY_MUL_MAX_BATCH*POLY_MUL_WORDS_PER_POLY
      m.write(s.read());
    }
    m.write_response();
  }

  // Stream <-> partitioned buffer. 
  static void unpack_poly(hls::stream<poly_word_t> &s, int16_t out[256]) {
    #pragma HLS INLINE off
    unpack_loop: for (int w = 0; w < POLY_MUL_WORDS_PER_POLY; w++) {
      #pragma HLS PIPELINE II=1
      poly_word_t v = s.read();
      for (int k = 0; k < POLY_MUL_COEFFS_PER_WORD; k++) {
        #pragma HLS UNROLL
        out[w * POLY_MUL_COEFFS_PER_WORD + k] =
            (int16_t)(uint16_t)v.range(16 * k + 15, 16 * k);
      }
    }
  }

  static void pack_poly(const int16_t in[256], hls::stream<poly_word_t> &s) {
    #pragma HLS INLINE off
    pack_loop: for (int w = 0; w < POLY_MUL_WORDS_PER_POLY; w++) {
      #pragma HLS PIPELINE II=1
      poly_word_t v;
      for (int k = 0; k < POLY_MUL_COEFFS_PER_WORD; k++) {
        #pragma HLS UNROLL
        v.range(16 * k + 15, 16 * k) =
            (ap_uint<16>)(uint16_t)in[w * POLY_MUL_COEFFS_PER_WORD + k];
      }
      s.write(v);
    }
  }

  //One polynomial per iteration, operands taken from the prefetch streams.
  static void compute_all(hls::stream<poly_word_t> &sa,
                          hls::stream<poly_word_t> &sb,
                          hls::stream<poly_word_t> &sr,
                          unsigned batch_count) {
    #pragma HLS INLINE off
    batch_loop:
    for (unsigned n = 0; n < batch_count; ++n) {
      #pragma HLS DATAFLOW
      #pragma HLS LOOP_TRIPCOUNT min=1 max=POLY_MUL_MAX_BATCH
      int16_t la[256], lb[256], lr[256];
      #pragma HLS ARRAY_PARTITION variable=la cyclic factor=8 dim=1
      #pragma HLS ARRAY_PARTITION variable=lb cyclic factor=8 dim=1
      #pragma HLS ARRAY_PARTITION variable=lr cyclic factor=8 dim=1

      unpack_poly(sa, la);
      unpack_poly(sb, lb);
      poly_mul(la, lb, lr);
      pack_poly(lr, sr);
    }
  }

  /*************************************************
  * Name:        poly_mul_batch
  *
  * Description: Up to POLY_MUL_MAX_BATCH independent products per dispatch.
  *
  *              Batching to amortise overhead, increasing throughput. A dispatch costs ~35 us
  *              measured on the board against ~4.7 us of compute, so the
  *              single-polynomial top is ~88% overhead; one dispatch over
  *              BATCH multiplies divides that by BATCH.
  *
  * Arguments:   - a, b: packed operands, polynomial n at words [n*8, n*8+8)
  *              - r: packed results, same layout
  **************************************************/
  void poly_mul_batch(hls::burst_maxi<poly_word_t> a,
                      hls::burst_maxi<poly_word_t> b,
                      hls::burst_maxi<poly_word_t> r,
                      unsigned batch_count) {
    /* Clamp before an AXI request, so even a bad caller can't make
       this kernel access beyond its synthesized maximum */
    unsigned count = batch_count;
    if (count < 1)
      count = 1;
    else if (count > POLY_MUL_MAX_BATCH)
      count = POLY_MUL_MAX_BATCH;

    #pragma HLS DATAFLOW
    hls::stream<poly_word_t> sa, sb, sr;
    /* Deep enough to cover a memory round trip without stalling the compute
       stage; 32 words is 4 polynomials of slack per operand. */
    #pragma HLS STREAM variable=sa depth=32
    #pragma HLS STREAM variable=sb depth=32
    #pragma HLS STREAM variable=sr depth=32

    read_all(a, sa, count);
    read_all(b, sb, count);
    compute_all(sa, sb, sr, count);
    write_all(r, sr, count);
  }
