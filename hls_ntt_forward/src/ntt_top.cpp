#include <stdint.h>
#include "params.h"
#include "ntt_top.h"
#include "reduction.h"
#include "ap_int.h"

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

static int16_t fqmul(int16_t a, int16_t b) {
  #pragma HLS INLINE
  return montgomery_reduce((int32_t)a*b);
}

template <int LEN>
static void ntt_stage(const int16_t in[256], int16_t out[256]) {
  #pragma HLS INLINE off
  for (int i = 0; i < 128; i++) {
    #pragma HLS PIPELINE II=1
    // 32 parallel butterfly units
    #pragma HLS UNROLL factor=32
    int g     = i / LEN;
    int off   = i % LEN;
    int start = g * (LEN << 1);
    int j     = start + off;
    int16_t zeta = zetas[128 / LEN + g];

    int16_t a = in[j];
    int16_t b = in[j + LEN];
    int16_t t = fqmul(zeta, b);
    out[j]       = a + t;
    out[j + LEN] = a - t;
  }
}

void ntt(int16_t r[256]) {
  #pragma HLS DATAFLOW

  int16_t buf1[256], buf2[256], buf3[256];
  int16_t buf4[256], buf5[256], buf6[256];
  
  #pragma HLS ARRAY_PARTITION variable=buf1 cyclic factor=64 dim=1
  #pragma HLS ARRAY_PARTITION variable=buf2 cyclic factor=64 dim=1
  #pragma HLS ARRAY_PARTITION variable=buf3 cyclic factor=64 dim=1
  #pragma HLS ARRAY_PARTITION variable=buf4 cyclic factor=64 dim=1
  #pragma HLS ARRAY_PARTITION variable=buf5 cyclic factor=64 dim=1
  #pragma HLS ARRAY_PARTITION variable=buf6 cyclic factor=64 dim=1
  // dataflow style
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
    if (SCALE) {                          
      s = fqmul(s, f);
      d = fqmul(d, f);
    }
    out[j]       = s;
    out[j + LEN] = d;
  }
}

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

  invntt_stage<2,   false>(r,    buf1);
  invntt_stage<4,   false>(buf1, buf2);
  invntt_stage<8,   false>(buf2, buf3);
  invntt_stage<16,  false>(buf3, buf4);
  invntt_stage<32,  false>(buf4, buf5);
  invntt_stage<64,  false>(buf5, buf6);
  invntt_stage<128, true> (buf6, r);
}

void basemul(int16_t r[2], const int16_t a[2], const int16_t b[2], int16_t zeta)
{
  r[0]  = fqmul(a[1], b[1]);
  r[0]  = fqmul(r[0], zeta);
  r[0] += fqmul(a[0], b[0]);
  r[1]  = fqmul(a[0], b[1]);
  r[1] += fqmul(a[1], b[0]);
}