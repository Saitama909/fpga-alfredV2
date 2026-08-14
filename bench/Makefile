# Software baseline for poly_mul.
#
# Needs an unmodified pq-crystals checkout:
#   git clone https://github.com/pq-crystals/kyber
#   make KYBER=/path/to/kyber/ref
#
# The reference (ntt.c, reduce.c) is compiled as C with the same flags
# pq-crystals uses, and linked in -- the harness being C++ must not change how
# the code under test is built. Only ntt.c and reduce.c are needed; the basemul
# loop is inlined in the harness so poly.c and its dependencies are not pulled in.
#
# Run on the KV260's A53 -- an x86 number is a harness smoke test, not the
# baseline. Pin the core and record the clock:
#   make env
#   taskset -c 0 ./bench_poly_mul

KYBER    ?= ../../kyber/ref
CC       ?= cc
CXX      ?= c++
CFLAGS   ?= -O3 -fomit-frame-pointer -Wall -Wextra
CXXFLAGS ?= -O3 -fomit-frame-pointer -Wall -Wextra
CPPFLAGS += -DKYBER_K=3 -I$(KYBER)

OBJS = bench_poly_mul.o ntt.o reduce.o

bench_poly_mul: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

bench_poly_mul.o: bench_poly_mul.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

ntt.o: $(KYBER)/ntt.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

reduce.o: $(KYBER)/reduce.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

.PHONY: clean env
clean:
	rm -f bench_poly_mul $(OBJS)

# Record alongside the result -- the number is not interpretable without it.
env:
	@$(CXX) --version | head -1
	@uname -srm
	@lscpu 2>/dev/null | grep -Ei "model name|architecture|mhz" || true
	@cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true
