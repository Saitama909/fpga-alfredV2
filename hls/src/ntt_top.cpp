#include <stdint.h>
#include "params.h"
#include "ntt_top.h"
#include "reduction.h"

/* How the zetas table was generated (from the reference Kyber code):

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
  for (i = 1; i < 128; i++)
    tmp[i] = fqmul(tmp[i - 1], MONT * KYBER_ROOT_OF_UNITY % KYBER_Q);

  for (i = 0; i < 128; i++) {
    zetas[i] = tmp[tree[i]];
    if (zetas[i] > KYBER_Q / 2)
      zetas[i] -= KYBER_Q;
    if (zetas[i] < -KYBER_Q / 2)
      zetas[i] += KYBER_Q;
  }
}
*/

/* Precomputed twiddle factors for the forward / inverse NTT. */
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

/*
 * Multiply then Montgomery-reduce: a * b * R^{-1} mod q.
 * Inlined + DSP-bound so HLS can fold this into the butterfly pipeline.
 */
static int16_t fqmul(int16_t a, int16_t b) {
  #pragma HLS INLINE
  int32_t prod = (int32_t)a * (int32_t)b;
  #pragma HLS BIND_OP variable=prod op=mul impl=dsp
  return montgomery_reduce(prod);
}

/*
 * Which forward-NTT schedule to build:
 *   1 = out-of-place ping-pong (best QoR so far: about 5382 cycles, 67% LUT)
 *   0 = older in-place path (about 27.6k cycles, 27% LUT)
 *
 * See OPTIMISATION_REPORT.md for the full story.
 */
#ifndef NTT_USE_PINGPONG
#define NTT_USE_PINGPONG 1
#endif

/* How many coeffs we copy per beat on the way in/out of the local buffers. */
static const unsigned int COPY_PARALLEL = 8;

#if NTT_USE_PINGPONG

/*
 * Butterflies issued together inside one group (same zeta).
 * We settled on 2: bumping to 4 with group unroll 2 made latency worse.
 * Group unroll factor is the literal 2 on the pragma below (HLS is happier
 * with a constant there than a named one).
 */
static const unsigned int NTT_PARALLEL = 2;

/*
 * One Cooley-Tukey stage, out of place.
 * Reads every element of src once and writes every element of dst once.
 * Groups in a stage don't touch the same indices, so we can unroll a couple.
 */
static void ntt_stage(int16_t dst[256], const int16_t src[256],
                      unsigned int len, unsigned int &k) {
  #pragma HLS INLINE

  const unsigned int n_groups = 128 / len;
  const unsigned int k_base = k;
  k = k_base + n_groups;

  group_loop: for (unsigned int g = 0; g < n_groups; g++) {
    #pragma HLS LOOP_TRIPCOUNT min=1 max=64
    #pragma HLS UNROLL factor=2
    const unsigned int start = g * (len << 1);
    const int16_t zeta = zetas[k_base + g];

    butterfly_loop: for (unsigned int j = start; j < start + len; j += NTT_PARALLEL) {
      #pragma HLS PIPELINE II=1
      #pragma HLS LOOP_TRIPCOUNT min=1 max=64
      for (unsigned int p = 0; p < NTT_PARALLEL; p++) {
        #pragma HLS UNROLL
        if (j + p < start + len) {
          const unsigned int jj = j + p;
          const int16_t t = fqmul(zeta, src[jj + len]);
          dst[jj + len] = src[jj] - t;
          dst[jj] = src[jj] + t;
        }
      }
    }
  }
}

/*
 * Forward NTT (standard order in, bit-reversed out).
 *
 * Ping-pong between two fully partitioned buffers so each stage can read
 * one and write the other without in-place hazards. Seven explicit stage
 * calls (len = 128 down to 2) keep the geometry obvious to HLS; after an
 * odd number of swaps the answer lives in buf_b.
 */
void ntt(int16_t r[256]) {
  int16_t buf_a[256];
  int16_t buf_b[256];
  #pragma HLS ARRAY_PARTITION variable=buf_a complete dim=1
  #pragma HLS ARRAY_PARTITION variable=buf_b complete dim=1

  /* Pull the polynomial into local storage. */
  copy_in: for (int i = 0; i < 256; i += COPY_PARALLEL) {
    #pragma HLS PIPELINE II=1
    for (unsigned int p = 0; p < COPY_PARALLEL; p++) {
      #pragma HLS UNROLL
      buf_a[i + p] = r[i + p];
    }
  }

  /* zetas[0] is unused on the forward path, same as the reference. */
  unsigned int k = 1;

  ntt_stage(buf_b, buf_a, 128, k);
  ntt_stage(buf_a, buf_b, 64, k);
  ntt_stage(buf_b, buf_a, 32, k);
  ntt_stage(buf_a, buf_b, 16, k);
  ntt_stage(buf_b, buf_a, 8, k);
  ntt_stage(buf_a, buf_b, 4, k);
  ntt_stage(buf_b, buf_a, 2, k);

  /* Write the result back out. */
  copy_out: for (int i = 0; i < 256; i += COPY_PARALLEL) {
    #pragma HLS PIPELINE II=1
    for (unsigned int p = 0; p < COPY_PARALLEL; p++) {
      #pragma HLS UNROLL
      r[i + p] = buf_b[i + p];
    }
  }
}

#else /* NTT_USE_PINGPONG == 0 */

/*
 * Older in-place forward NTT. Kept around as a fallback / comparison point.
 * Fully partitioned local_r, four butterflies per beat, wide copies.
 * About 27.6k cycles and 27% LUT on the last synth we ran.
 */
void ntt(int16_t r[256]) {
  unsigned int len, start, j, k;
  int16_t zeta;
  const unsigned int PARALLEL = 4;

  int16_t local_r[256];
  #pragma HLS ARRAY_PARTITION variable=local_r complete dim=1

  copy_in: for (int i = 0; i < 256; i += COPY_PARALLEL) {
    #pragma HLS PIPELINE II=1
    for (unsigned int p = 0; p < COPY_PARALLEL; p++) {
      #pragma HLS UNROLL
      local_r[i + p] = r[i + p];
    }
  }

  k = 1;
  stage_loop: for (len = 128; len >= 2; len >>= 1) {
    #pragma HLS LOOP_TRIPCOUNT min=7 max=7
    group_loop: for (start = 0; start < 256; start += (len << 1)) {
      #pragma HLS LOOP_TRIPCOUNT min=1 max=64
      zeta = zetas[k++];
      butterfly_loop: for (j = start; j < start + len; j += PARALLEL) {
        #pragma HLS PIPELINE II=1
        #pragma HLS DEPENDENCE variable=local_r type=inter dependent=false
        #pragma HLS LOOP_TRIPCOUNT min=1 max=32
        for (unsigned int p = 0; p < PARALLEL; p++) {
          #pragma HLS UNROLL
          if (j + p < start + len) {
            const unsigned int jj = j + p;
            const int16_t t = fqmul(zeta, local_r[jj + len]);
            local_r[jj + len] = local_r[jj] - t;
            local_r[jj] = local_r[jj] + t;
          }
        }
      }
    }
  }

  copy_out: for (int i = 0; i < 256; i += COPY_PARALLEL) {
    #pragma HLS PIPELINE II=1
    for (unsigned int p = 0; p < COPY_PARALLEL; p++) {
      #pragma HLS UNROLL
      r[i + p] = local_r[i + p];
    }
  }
}

#endif /* NTT_USE_PINGPONG */

/*
 * Inverse NTT, then scale by the Montgomery factor 2^16.
 * Bit-reversed in, standard order out. Still the reference structure;
 * we haven't spent the same HLS effort here as on the forward path.
 */
void invntt(int16_t r[256]) {
  unsigned int start, len, j, k;
  int16_t t, zeta;
  const int16_t f = 1441; /* mont^2 / 128 */

  k = 127;
  for (len = 2; len <= 128; len <<= 1) {
    for (start = 0; start < 256; start = j + len) {
      zeta = zetas[k--];
      for (j = start; j < start + len; j++) {
        t = r[j];
        r[j] = barrett_reduce(t + r[j + len]);
        r[j + len] = r[j + len] - t;
        r[j + len] = fqmul(zeta, r[j + len]);
      }
    }
  }

  for (j = 0; j < 256; j++)
    r[j] = fqmul(r[j], f);
}

/*
 * Multiply two degree-1 polys in Zq[X] / (X^2 - zeta).
 * Used for pointwise products once you're in the NTT domain.
 */
void basemul(int16_t r[2], const int16_t a[2], const int16_t b[2], int16_t zeta) {
  r[0] = fqmul(a[1], b[1]);
  r[0] = fqmul(r[0], zeta);
  r[0] += fqmul(a[0], b[0]);
  r[1] = fqmul(a[0], b[1]);
  r[1] += fqmul(a[1], b[0]);
}
