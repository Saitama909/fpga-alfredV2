#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "../src/params.h"
#include "../src/ntt_top.h"

#define Q KYBER_Q

static int16_t center_mod_q(int64_t x) {
  int64_t r = x % Q;
  if (r < 0) r += Q;
  if (r > Q / 2) r -= Q;
  return (int16_t)r;
}

// Smoke test: kernel runs and writes something other than the poison value.
// Full correctness is checked on the board against the host schoolbook path.
int main() {
  static int16_t A_T[KYBER_K][KYBER_K][256];
  static int16_t t_hat[KYBER_K][256];
  static int16_t r[KYBER_K][256], e1[KYBER_K][256], e2[256], msg[256];
  static int16_t u[KYBER_K][256], v[256];

  srand(42);
  for (int i = 0; i < KYBER_K; i++)
    for (int j = 0; j < KYBER_K; j++)
      for (int c = 0; c < 256; c++)
        A_T[i][j][c] = center_mod_q(rand() % Q);
  for (int j = 0; j < KYBER_K; j++)
    for (int c = 0; c < 256; c++) {
      t_hat[j][c] = center_mod_q(rand() % Q);
      r[j][c] = (rand() % 5) - 2;
      e1[j][c] = (rand() % 5) - 2;
    }
  for (int c = 0; c < 256; c++) {
    e2[c] = (rand() % 5) - 2;
    msg[c] = (rand() & 1) ? (int16_t)((Q + 1) / 2) : 0;
    v[c] = 0x7fff;
  }
  for (int i = 0; i < KYBER_K; i++)
    for (int c = 0; c < 256; c++)
      u[i][c] = 0x7fff;

  kyber_enc_core(A_T, t_hat, r, e1, e2, msg, u, v);

  int touched = 0;
  for (int i = 0; i < KYBER_K; i++)
    for (int c = 0; c < 256; c++)
      if (u[i][c] != 0x7fff) touched++;
  for (int c = 0; c < 256; c++)
    if (v[c] != 0x7fff) touched++;

  if (touched == 0) {
    printf("FAIL: kyber_enc_core left outputs untouched\n");
    return 1;
  }
  printf("PASS: kyber_enc_core smoke (touched %d coeffs)\n", touched);
  return 0;
}
