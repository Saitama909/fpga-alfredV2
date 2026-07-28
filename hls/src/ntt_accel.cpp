#include "ntt_accel.h"
#include "ntt_top.h"

extern "C" void ntt_accel(int16_t r[256]) {
#pragma HLS INTERFACE m_axi port=r offset=slave bundle=gmem \
    depth=256 max_widen_bitwidth=512
#pragma HLS INTERFACE s_axilite port=r bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    ntt(r);
}
