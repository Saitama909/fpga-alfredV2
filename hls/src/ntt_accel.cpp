#include "ntt_accel.h"
#include "ntt_top.h"

extern "C" void ntt_accel(int16_t r[256]) {
#pragma HLS INTERFACE m_axi port=r offset=slave bundle=gmem \
    depth=256 max_widen_bitwidth=512 max_read_burst_length=256 max_write_burst_length=256
#pragma HLS INTERFACE s_axilite port=r bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    int16_t local_r[256];
#pragma HLS ARRAY_PARTITION variable=local_r cyclic factor=32 dim=1

read_burst:
    for (int i = 0; i < 256; ++i) {
#pragma HLS PIPELINE II=1
        local_r[i] = r[i];
    }

    ntt(local_r);

write_burst:
    for (int i = 0; i < 256; ++i) {
#pragma HLS PIPELINE II=1
        r[i] = local_r[i];
    }
}
