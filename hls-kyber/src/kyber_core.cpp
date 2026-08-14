// ============================================================================
// kyber_core.cpp -- NTT-domain stage of Kyber encryption (kyber_enc_core).
// ============================================================================

#include <stdint.h>
#include "params.h"
#include "ntt_top.h"

// Wills original cyclic-8. ENC_MEM_PAR=16 met HLS but missed P&R by ~0.19 ns on the w→vl path
// 8 cuts fanout/routing enough to close timing.
static const int ENC_MEM_PAR = 8;

static void copy_poly(const int16_t in[256], int16_t out[256]) {
#pragma HLS INLINE off
    for (int i = 0; i < 256; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=ENC_MEM_PAR
        out[i] = in[i];
    }
}

static void copy_r(const int16_t r[KYBER_K][256], int16_t rl[KYBER_K][256]) {
#pragma HLS INLINE off
    for (int i = 0; i < KYBER_K; i++)
        copy_poly(r[i], rl[i]);
}

static void copy_A(const int16_t A_T[KYBER_K][KYBER_K][256],
                   int16_t Al[KYBER_K][KYBER_K][256]) {
#pragma HLS INLINE off
    for (int i = 0; i < KYBER_K; i++)
        for (int j = 0; j < KYBER_K; j++)
            copy_poly(A_T[i][j], Al[i][j]);
}

static void copy_t(const int16_t t_hat[KYBER_K][256], int16_t tl[KYBER_K][256]) {
#pragma HLS INLINE off
    for (int i = 0; i < KYBER_K; i++)
        copy_poly(t_hat[i], tl[i]);
}

static void copy_e1(const int16_t e1[KYBER_K][256], int16_t e1l[KYBER_K][256]) {
#pragma HLS INLINE off
    for (int i = 0; i < KYBER_K; i++)
        copy_poly(e1[i], e1l[i]);
}

// ---------------------------------------------------------------------------
// ENCRYPTION
//
//   u = A^T . r + e1        (K polynomials)
//   v = t^T . r + e2 + m    (1 polynomial)
//
// A_T and t_hat arrive already in the NTT domain. r/e1/e2/msg are normal-domain.
// ---------------------------------------------------------------------------
void kyber_enc_core(
    const int16_t A_T   [KYBER_K][KYBER_K][256],
    const int16_t t_hat [KYBER_K][256],
    const int16_t r     [KYBER_K][256],
    const int16_t e1    [KYBER_K][256],
    const int16_t e2    [256],
    const int16_t msg   [256],
          int16_t u     [KYBER_K][256],
          int16_t v     [256])
{
    #pragma HLS INTERFACE ap_ctrl_chain port=return
    #pragma HLS INTERFACE mode=s_axilite port=return
    #pragma HLS DATAFLOW
    #pragma HLS INTERFACE mode=m_axi port=A_T   bundle=gmem0 depth=2304
    #pragma HLS INTERFACE mode=m_axi port=t_hat bundle=gmem1 depth=768
    #pragma HLS INTERFACE mode=m_axi port=r     bundle=gmem2 depth=768
    #pragma HLS INTERFACE mode=m_axi port=e1    bundle=gmem3 depth=768
    #pragma HLS INTERFACE mode=m_axi port=e2    bundle=gmem1 depth=256
    #pragma HLS INTERFACE mode=m_axi port=msg   bundle=gmem2 depth=256
    #pragma HLS INTERFACE mode=m_axi port=u     bundle=gmem4 depth=768
    #pragma HLS INTERFACE mode=m_axi port=v     bundle=gmem4 depth=256

    int16_t Al[KYBER_K][KYBER_K][256];
    int16_t tl[KYBER_K][256];
    int16_t rl[KYBER_K][256];
    int16_t e1l[KYBER_K][256];
    int16_t e2l[256];
    int16_t msgl[256];
    #pragma HLS ARRAY_PARTITION variable=Al   cyclic factor=ENC_MEM_PAR dim=3
    #pragma HLS ARRAY_PARTITION variable=tl   cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS ARRAY_PARTITION variable=rl   cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS ARRAY_PARTITION variable=e1l  cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS ARRAY_PARTITION variable=e2l  cyclic factor=ENC_MEM_PAR
    #pragma HLS ARRAY_PARTITION variable=msgl cyclic factor=ENC_MEM_PAR
    #pragma HLS BIND_STORAGE variable=Al  type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=tl  type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=rl  type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=e1l type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=e2l type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=msgl type=ram_2p impl=lutram

    int16_t r_hat[KYBER_K][256];
    int16_t acc[KYBER_K+1][256];
    int16_t w[KYBER_K+1][256];
    #pragma HLS ARRAY_PARTITION variable=r_hat cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS ARRAY_PARTITION variable=acc   cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS ARRAY_PARTITION variable=w     cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS BIND_STORAGE variable=r_hat type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=acc   type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=w     type=ram_2p impl=lutram

    int16_t ul[KYBER_K][256];
    int16_t vl[256];
    #pragma HLS ARRAY_PARTITION variable=ul cyclic factor=ENC_MEM_PAR dim=2
    #pragma HLS ARRAY_PARTITION variable=vl cyclic factor=ENC_MEM_PAR
    #pragma HLS BIND_STORAGE variable=ul type=ram_2p impl=lutram
    #pragma HLS BIND_STORAGE variable=vl type=ram_2p impl=lutram

    copy_r(r, rl);
    copy_A(A_T, Al);
    copy_t(t_hat, tl);
    copy_e1(e1, e1l);
    copy_poly(e2,  e2l);
    copy_poly(msg, msgl);

    for (int j = 0; j < KYBER_K; j++)
        hls_poly_ntt(rl[j], r_hat[j]);

    for (int i = 0; i < KYBER_K; i++)
        hls_polyvec_basemul_acc(acc[i], Al[i], r_hat);
    hls_polyvec_basemul_acc(acc[KYBER_K], tl, r_hat);

    for (int i = 0; i <= KYBER_K; i++)
        invntt(acc[i], w[i]);

    for (int i = 0; i < KYBER_K; i++) {
        hls_poly_add(ul[i], w[i], e1l[i]);
        hls_poly_reduce(ul[i]);
    }
    hls_poly_add(vl, w[KYBER_K], e2l);
    hls_poly_add(vl, vl, msgl);
    hls_poly_reduce(vl);

    for (int i = 0; i < KYBER_K; i++)
        copy_poly(ul[i], u[i]);
    copy_poly(vl, v);
}
