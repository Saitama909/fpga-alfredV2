#ifndef NTT_ACCEL_H
#define NTT_ACCEL_H

#include <stdint.h>
#include <ap_int.h>
#include "params.h"

#define NTT_ACCEL_BATCH_SIZE 128

typedef ap_uint<512> vec512_t;

extern "C" void ntt_accel(vec512_t* r);

#endif
