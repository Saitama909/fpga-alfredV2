// ============================================================================
// kyber_core.cpp -- minimal HLS for the NTT-domain stage of Kyber.
// ============================================================================

#include <stdint.h>
#include "params.h"
#include "ntt_top.h"

// Copy one polynomial. INLINE off so it shows as its own module in the
// report; UNROLL 8 needs the DESTINATION partitioned cyclic-8, which is why
// every local buffer below carries that pragma.
static void copy_poly(const int16_t in[256], int16_t out[256]) {
#pragma HLS INLINE off
    for (int i = 0; i < 256; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=8
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
// Both are "sum of K polynomial products", so the same accumulate function
// produces every output row -- HLS builds one datapath and reuses it.
//
// No tomont here: invntt already supplies the R that cancels basemul's R^-1.
//
// STAGING
//   Every interface array is copied into a partitioned local buffer before
//   use, and the outputs are copied back at the end. Interface ports have a
//   fixed access protocol the scheduler cannot rearrange, so an unrolled loop
//   reading or writing one directly builds N datapaths that all queue at a
//   single port -- more area, no more throughput. .
// ---------------------------------------------------------------------------
void kyber_enc_core(
    const int16_t A_T   [KYBER_K][KYBER_K][256],  // NTT domain, must be transposed
    const int16_t t_hat [KYBER_K][256],           // NTT domain, from pk
    const int16_t r     [KYBER_K][256],           // normal domain, CBD
    const int16_t e1    [KYBER_K][256],           // normal domain, CBD
    const int16_t e2    [256],                    // normal domain, CBD
    const int16_t msg   [256],                    // normal domain, frommsg
          int16_t u     [KYBER_K][256],           // out: normal, reduced
          int16_t v     [256])                    // out: normal, reduced
{
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
    #pragma HLS INTERFACE mode=s_axilite port=return

    // inputs, staged
    int16_t Al[KYBER_K][KYBER_K][256];
    int16_t tl[KYBER_K][256];
    int16_t rl[KYBER_K][256];
    int16_t e1l[KYBER_K][256];
    int16_t e2l[256];
    int16_t msgl[256];
    #pragma HLS ARRAY_PARTITION variable=Al   cyclic factor=8 dim=3
    #pragma HLS ARRAY_PARTITION variable=tl   cyclic factor=8 dim=2
    #pragma HLS ARRAY_PARTITION variable=rl   cyclic factor=8 dim=2
    #pragma HLS ARRAY_PARTITION variable=e1l  cyclic factor=8 dim=2
    #pragma HLS ARRAY_PARTITION variable=e2l  cyclic factor=8
    #pragma HLS ARRAY_PARTITION variable=msgl cyclic factor=8

    // working buffers
    int16_t r_hat[KYBER_K][256];
    int16_t acc[KYBER_K+1][256];
    int16_t w[KYBER_K+1][256];
    #pragma HLS ARRAY_PARTITION variable=r_hat cyclic factor=8 dim=2
    #pragma HLS ARRAY_PARTITION variable=acc   cyclic factor=8 dim=2
    #pragma HLS ARRAY_PARTITION variable=w     cyclic factor=8 dim=2

    // outputs, staged
    int16_t ul[KYBER_K][256];
    int16_t vl[256];
    #pragma HLS ARRAY_PARTITION variable=ul cyclic factor=8 dim=2
    #pragma HLS ARRAY_PARTITION variable=vl cyclic factor=8

    // 0. pull everything off the interface
    copy_r(r, rl);
    copy_A(A_T, Al);
    copy_t(t_hat, tl);
    copy_e1(e1, e1l);
    copy_poly(e2,  e2l);
    copy_poly(msg, msgl);

    // 1. transform the noise vector
    // ref: Corresponds to polyvec_ntt(&sp)
    for (int j = 0; j < KYBER_K; j++)
        hls_poly_ntt(rl[j], r_hat[j]);

    // 2. the multiply -- K+1 rows, K columns
    // ref: Corresponds to
    //         for(i=0;i<KYBER_K;i++)
    //           polyvec_basemul_acc_montgomery(&b.vec[i], &at[i], &sp); -> acc[0..K-1]
    //         polyvec_basemul_acc_montgomery(&v, &pkpv, &sp);           -> acc[K]
    // acc[i] = R⁻¹ · Σⱼ₌₀^{K-1} Âᵀ[i][j] ∘ r̂ⱼ     for i = 0 … K-1   (-> u)
    // acc[K] = R⁻¹ · Σⱼ₌₀^{K-1} t̂ⱼ      ∘ r̂ⱼ                        (-> v)
    for (int i = 0; i < KYBER_K; i++)
        hls_polyvec_basemul_acc(acc[i], Al[i], r_hat);
    hls_polyvec_basemul_acc(acc[KYBER_K], tl, r_hat);

    // 3. back to the normal domain
    //    ref: Corresponds to
    //      polyvec_invntt_tomont(&b);   -> w[0..K-1]
    //      poly_invntt_tomont(&v);      -> w[K]
    //  w[0..K-1] = Aᵀ·r  and  w[K] = tᵀ·r
    for (int i = 0; i <= KYBER_K; i++)
        invntt(acc[i], w[i]);

    // 4. add the noise. This happens AFTER invntt because u and v must be
    //    compressed next, and compression is nonlinear -- it cannot cross
    //    the transform.
    // ref: Corresponds to
    //         polyvec_add(&b, &b, &ep);    -> e1 into u
    //         poly_add(&v, &v, &epp);      -> e2 into v
    //         poly_add(&v, &v, &k);        -> message into v
    //         polyvec_reduce(&b);
    //         poly_reduce(&v);
    for (int i = 0; i < KYBER_K; i++) {
        hls_poly_add(ul[i], w[i], e1l[i]);
        hls_poly_reduce(ul[i]);
    }
    hls_poly_add(vl, w[KYBER_K], e2l);
    hls_poly_add(vl, vl, msgl);
    hls_poly_reduce(vl);

    // 5. push the results back out
    COPY_OUT:
        for (int i = 0; i < KYBER_K; i++)
            copy_poly(ul[i], u[i]);
        copy_poly(vl, v);
}