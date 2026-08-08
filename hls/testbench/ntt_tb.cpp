#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "../src/params.h"
#include "../src/reduction.h"
#include "../src/ntt_top.h"

// Centered representative in (-q/2, q/2]
static int16_t center_mod_q(int32_t x) {
  int32_t r = x % KYBER_Q;
  if (r < 0) r += KYBER_Q;
  if (r > KYBER_Q / 2) r -= KYBER_Q;
  return (int16_t)r;
}

// ---------------------------------------------------------------------------
// Test 1 -- NTT round trip (unchanged)
// ---------------------------------------------------------------------------
static int test_roundtrip() {
  int16_t orig[256], fwd[256], work[256];

  for (int i = 0; i < 256; i++)
    orig[i] = i - 128;

  // out-of-place, so orig unmodified and can be compared against
  ntt(orig, fwd);
  invntt(fwd, work);

  // invntt(ntt(r)) == r * MONT (mod q): invntt performs the inverse transform
  // AND multiplies by the Montgomery factor 2^16 mod q. Outputs are only
  // lazily reduced, so compare as residues rather than requiring equality.
  int errors = 0;
  for (int i = 0; i < 256; i++) {
    int16_t expected = center_mod_q((int32_t)orig[i] * MONT);
    int16_t got = center_mod_q(work[i]);
    if (got != expected) {
      if (errors < 10)
        printf("MISMATCH at %d: got %d (raw %d), expected %d (orig %d)\n",
               i, got, work[i], expected, orig[i]);
      errors++;
    }
  }
  printf("Test 1 invntt(ntt(r)) == r * MONT : %s (%d mismatches)\n",
         errors ? "FAIL" : "PASS", errors);
  return errors;
}

// ---------------------------------------------------------------------------
// Stubs for stages 1, 2 and 4.
//
// A is DEFINED as uniform NTT-domain values, so drawing them from rand() is
// distributionally identical to drawing them from SHAKE-128 plus rejection
// sampling -- kyber_enc_core cannot tell the difference. Compression is
// skipped so any failure is unambiguously an arithmetic bug rather than a
// rounding-noise event.
// ---------------------------------------------------------------------------
#define ETA 2

static void fake_uniform_ntt(int16_t p[256]) {
  for (int i = 0; i < 256; i++) p[i] = center_mod_q(rand() % KYBER_Q);
}

static void fake_cbd(int16_t p[256]) {
  for (int i = 0; i < 256; i++) {
    int acc = 0;
    for (int k = 0; k < ETA; k++) acc += (rand() & 1) - (rand() & 1);
    p[i] = (int16_t)acc;
  }
}

// Each message bit becomes 0 or ceil(q/2). Decoding picks whichever is nearer,
// so a coefficient can drift almost q/4 = 832 and still decode correctly.
// This is the error correction the whole scheme relies on.
static void msg_to_poly(int16_t p[256], const uint8_t msg[32]) {
  for (int i = 0; i < 32; i++)
    for (int j = 0; j < 8; j++) {
      int16_t mask = -(int16_t)((msg[i] >> j) & 1);
      p[8*i + j] = mask & (int16_t)((KYBER_Q + 1) / 2);
    }
}

static void poly_to_msg(uint8_t msg[32], const int16_t p[256]) {
  memset(msg, 0, 32);
  for (int i = 0; i < 32; i++)
    for (int j = 0; j < 8; j++) {
      int32_t t = p[8*i + j];
      if (t < 0) t += KYBER_Q;
      t = (((t << 1) + KYBER_Q / 2) / KYBER_Q) & 1;
      msg[i] |= (uint8_t)(t << j);
    }
}

// ---------------------------------------------------------------------------
// Test 2 -- full round trip through kyber_enc_core.
//
// There is no golden model here. The check is that the s^T.A^T.r term appears
// in both v and s^T.u, computed by completely different routes, and cancels
// exactly. If any function on either path is wrong the two differ by roughly
// q/2, which blows past the q/4 decode margin and the message comes back as
// noise.
//
// ---------------------------------------------------------------------------
static int test_encrypt() {
  int errors = 0;
  const int TRIALS = 10;

  for (int trial = 0; trial < TRIALS; trial++) {
    int16_t A[KYBER_K][KYBER_K][256], A_T[KYBER_K][KYBER_K][256];
    int16_t s[KYBER_K][256], e[KYBER_K][256];
    int16_t s_hat[KYBER_K][256], e_hat[KYBER_K][256];
    int16_t t_hat[KYBER_K][256];
    int16_t r[KYBER_K][256], e1[KYBER_K][256], e2[256], k[256];
    int16_t u[KYBER_K][256], v[256];
    int16_t acc[256], mp[256], w[256];
    int16_t u_hat[KYBER_K][256];
    uint8_t msg[32], got[32];

    // ---- keygen: t = A.s + e, staying in the NTT domain ----
    for (int i = 0; i < KYBER_K; i++)
      for (int j = 0; j < KYBER_K; j++)
        fake_uniform_ntt(A[i][j]);

    for (int j = 0; j < KYBER_K; j++) {
      fake_cbd(s[j]);  hls_poly_ntt(s[j], s_hat[j]);
      fake_cbd(e[j]);  hls_poly_ntt(e[j], e_hat[j]);
    }

    for (int i = 0; i < KYBER_K; i++) {
      hls_polyvec_basemul_acc(t_hat[i], A[i], s_hat);
      hls_poly_tomont(t_hat[i]);                    // no invntt here, so multiply by R
      hls_poly_add(t_hat[i], t_hat[i], e_hat[i]);   // the R^-1 debt 
      hls_poly_reduce(t_hat[i]);
    }

    // encryption needs A transposed. The real sampler does this by swapping
    // the (i,j) domain separators, not by transposing a matrix.
    for (int i = 0; i < KYBER_K; i++)
      for (int j = 0; j < KYBER_K; j++)
        memcpy(A_T[i][j], A[j][i], 512);

    // ---- encrypt ----
    for (int j = 0; j < KYBER_K; j++) { fake_cbd(r[j]); fake_cbd(e1[j]); }
    fake_cbd(e2);
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)(rand() & 0xFF);
    msg_to_poly(k, msg);

    kyber_enc_core(A_T, t_hat, r, e1, e2, k, u, v);

    // ---- decrypt: m' = v - s^T.u ----
    for (int j = 0; j < KYBER_K; j++)
      hls_poly_ntt(u[j], u_hat[j]);
    hls_polyvec_basemul_acc(acc, s_hat, u_hat);
    invntt(acc, w);
    hls_poly_sub(mp, v, w);
    hls_poly_reduce(mp);
    poly_to_msg(got, mp);

    if (memcmp(msg, got, 32) != 0) {
      int bad = 0;
      for (int i = 0; i < 32; i++) {
        uint8_t x = msg[i] ^ got[i];
        while (x) { bad += x & 1; x >>= 1; }
      }
      if (errors < 3)
        printf("  trial %d: %d of 256 bits wrong\n", trial, bad);
      errors++;
    }
  }

  printf("Test 2 encrypt/decrypt round trip     : %s (%d of %d trials failed)\n",
         errors ? "FAIL" : "PASS", errors, TRIALS);
  if (errors)
    printf("  around 128 bits wrong means the cancellation failed entirely --\n"
           "  look for a scaling error or a transposed A, not a noise problem\n");
  return errors;
}

int main() {
  srand(42);
  int e = 0;
  e += test_roundtrip();
  e += test_encrypt();
  printf("%s\n", e ? "=== FAILED ===" : "=== ALL PASSED ===");
  return e ? 1 : 0;
}