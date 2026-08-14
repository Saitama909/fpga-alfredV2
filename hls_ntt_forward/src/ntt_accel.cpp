#include <stdint.h>
#include <hls_stream.h>
#include <ap_int.h>
#include "ntt_accel.h"
#include "ntt_top.h"

static void read_burst_func(
    const vec512_t* src,
    hls::stream<vec512_t>& in_stream,
    int num_jobs
) {
read_loop:
    for (int j = 0; j < num_jobs; ++j) {
        int base = j * (KYBER_N / 32); // 256 / 32 = 8 words per job
    read_words:
        for (int w = 0; w < (KYBER_N / 32); ++w) {
#pragma HLS PIPELINE II=1
            in_stream.write(src[base + w]);
        }
    }
}

static void compute_func(
    hls::stream<vec512_t>& in_stream,
    hls::stream<vec512_t>& out_stream,
    int num_jobs
) {
compute_loop:
    for (int j = 0; j < num_jobs; ++j) {
        int16_t local_r[KYBER_N];
#pragma HLS ARRAY_PARTITION variable=local_r cyclic factor=64 dim=1

    unpack_loop:
        for (int w = 0; w < (KYBER_N / 32); ++w) {
#pragma HLS PIPELINE II=1
            vec512_t word = in_stream.read();
            for (int k = 0; k < 32; ++k) {
#pragma HLS UNROLL
                local_r[w * 32 + k] = word.range((k + 1) * 16 - 1, k * 16);
            }
        }

        ntt(local_r);

    pack_loop:
        for (int w = 0; w < (KYBER_N / 32); ++w) {
#pragma HLS PIPELINE II=1
            vec512_t word = 0;
            for (int k = 0; k < 32; ++k) {
#pragma HLS UNROLL
                word.range((k + 1) * 16 - 1, k * 16) = static_cast<uint16_t>(local_r[w * 32 + k]);
            }
            out_stream.write(word);
        }
    }
}

static void write_burst_func(
    vec512_t* dst,
    hls::stream<vec512_t>& out_stream,
    int num_jobs
) {
write_loop:
    for (int j = 0; j < num_jobs; ++j) {
        int base = j * (KYBER_N / 32);
    write_words:
        for (int w = 0; w < (KYBER_N / 32); ++w) {
#pragma HLS PIPELINE II=1
            dst[base + w] = out_stream.read();
        }
    }
}

extern "C" void ntt_accel(vec512_t* r) {
#pragma HLS INTERFACE m_axi port=r offset=slave bundle=gmem \
    depth=1024 \
    max_widen_bitwidth=512 \
    max_read_burst_length=256 \
    max_write_burst_length=256

#pragma HLS INTERFACE s_axilite port=r bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

#pragma HLS DATAFLOW

    hls::stream<vec512_t> in_stream("in_stream");
#pragma HLS STREAM variable=in_stream depth=16

    hls::stream<vec512_t> out_stream("out_stream");
#pragma HLS STREAM variable=out_stream depth=16

    read_burst_func(r, in_stream, NTT_ACCEL_BATCH_SIZE);
    compute_func(in_stream, out_stream, NTT_ACCEL_BATCH_SIZE);
    write_burst_func(r, out_stream, NTT_ACCEL_BATCH_SIZE);
}
