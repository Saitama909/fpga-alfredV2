#include "ntt_accel.h"
#include "ntt_top.h"

extern "C" void ntt_accel(int16_t r[256]) {
// 32 16-bit coefficients per AXI transfer word, so drops 256-element read burst cycles from 128 cycles down to 8 cycles!

// // notes for myself - to allow FPGA to do DDR requests/recieve data nd what not.
#pragma HLS INTERFACE m_axi port=r offset=slave bundle=gmem \ // Base address supplied by AXI-Lite slave reg
    depth=256 max_widen_bitwidth=512 max_read_burst_length=256 max_write_burst_length=256

    // AXI-Lite Slave Control Interface Pragmas:
    // Exposes control register 0x00 (ap_start/ap_done) and 0x10/0x14 (pointer r)
    
// AXI lite slave interface to write and read
#pragma HLS INTERFACE s_axilite port=r bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

// BRAm array 
// Cyclic factor=32 partitions memory into 32 parallel BRAM 
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
