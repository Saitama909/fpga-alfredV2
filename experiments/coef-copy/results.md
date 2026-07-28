# Coef-copy width sweep results
> Riley

## Motivation

`ntt()` func copies 256 coeffs into a fully partitioned `local_r`, runs butterflies, then copy back. 

At width 1 that memcpy alone costs about 512 cycles. Widening the copy (N coeffs/cycle) reduces overhead without changing the NTT math.

## Method

- Swept all divisors of 256: `1, 2, 4, 8, 16, 32, 64, 128, 256`
- Target clock: **5.00 ns** (est. **3.650 ns** across all widths)

## Results

| width | csynth (cyc) | cosim (cyc) | Diff | abs lat (µs) | BRAM % | DSP % | FF % | LUT % | csim | cosim |
|------:|-------------:|------------:|--:|-------------:|-------:|------:|-----:|------:|:----:|:-----:|
| 1     | 976          | 965         | −11 | 4.880      | 10     | 29    | 25   | 71    | PASS | PASS |
| 2     | 720          | 709         | −11 | 3.600      | 10     | 29    | 25   | 71    | PASS | PASS |
| 4     | 592          | 581         | −11 | 2.960      | 10     | 29    | 25   | 71    | PASS | PASS |
| 8     | 528          | 517         | −11 | 2.640      | 10     | 29    | 25   | 71    | PASS | PASS |
| 16    | 496          | 485         | −11 | 2.480      | 10     | 29    | 25   | 71    | PASS | PASS |
| 32    | 480          | 469         | −11 | 2.400      | 10     | 29    | 25   | 71    | PASS | PASS |
| 64    | 480          | 469         | −11 | 2.400      | 10     | 29    | 25   | 71    | PASS | PASS |
| 128   | 480          | 469         | −11 | 2.400      | 10     | 29    | 26   | 76    | PASS | PASS |
| 256   | 471          | 466         | −5  | 2.355      | 10     | 29    | 25   | 67    | PASS | PASS |

> BRAM 29/288 and DSP 372/1248 were constant across the sweep.

## Takeaway

Most of the win is by **32**. **64/128** match width-32 latency. **128** costs more LUTs (76%). **256** is only a few cycles better with LUT 67%. Cosim tracks csynth closely (usually **11** cycles under the estimate).
