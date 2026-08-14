#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "../src/ntt_accel.h"
#include "../src/params.h"
#include "../src/reduction.h"
#include "../src/ntt_top.h"

static int16_t center_mod_q(int32_t x) {
    int32_t r = x % KYBER_Q;
    if (r < 0) r += KYBER_Q;
    if (r > KYBER_Q / 2) r -= KYBER_Q;
    return (int16_t)r;
}

int main() {
    static int16_t original[NTT_ACCEL_BATCH_SIZE * KYBER_N];
    static int16_t work[NTT_ACCEL_BATCH_SIZE * KYBER_N];
    static int16_t expected[NTT_ACCEL_BATCH_SIZE * KYBER_N];

    // Generate 128 different polynomials
    for (int job = 0; job < NTT_ACCEL_BATCH_SIZE; ++job) {
        for (int i = 0; i < KYBER_N; ++i) {
            const int idx = job * KYBER_N + i;
            original[idx] = (int16_t)(((i + 17 * job) % 256) - 128);
            work[idx] = original[idx];
            expected[idx] = original[idx];
        }
        // Compute direct forward NTT reference for each polynomial
        ntt(&expected[job * KYBER_N]);
    }

    // Run hardware accelerated batch kernel
    ntt_accel((vec512_t*)work);

    // 1. Direct Forward NTT Bit-Exact Comparison against C reference (ntt_top.h)
    int fwd_errors = 0;
    for (int idx = 0; idx < NTT_ACCEL_BATCH_SIZE * KYBER_N; ++idx) {
        if (work[idx] != expected[idx]) {
            if (fwd_errors < 10) {
                printf("FORWARD MISMATCH job=%d coeff=%d: got %d, expected %d\n",
                       idx / KYBER_N, idx % KYBER_N, work[idx], expected[idx]);
            }
            fwd_errors++;
        }
    }

    // 2. Inverse NTT Roundtrip Verification (invntt(ntt(r)) == r * MONT mod q)
    int inv_errors = 0;
    for (int job = 0; job < NTT_ACCEL_BATCH_SIZE; ++job) {
        int16_t *poly = &work[job * KYBER_N];
        invntt(poly);
        for (int i = 0; i < KYBER_N; ++i) {
            const int idx = job * KYBER_N + i;
            int16_t exp_val = center_mod_q((int32_t)original[idx] * MONT);
            int16_t got_val = center_mod_q(poly[i]);
            if (got_val != exp_val) {
                if (inv_errors < 10) {
                    printf("INVERSE MISMATCH job=%d coeff=%d: got %d, expected %d\n",
                           job, i, got_val, exp_val);
                }
                inv_errors++;
            }
        }
    }

    if (fwd_errors == 0 && inv_errors == 0) {
        printf("PASS: Direct Forward NTT reference match & Inverse NTT verification for all %d polynomials!\n",
               NTT_ACCEL_BATCH_SIZE);
        return 0;
    }

    printf("FAIL: %d forward errors, %d inverse errors\n", fwd_errors, inv_errors);
    return 1;
}
