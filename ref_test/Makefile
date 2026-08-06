CC ?= /usr/bin/cc
CXX ?= /usr/bin/c++
CFLAGS += -Wall -Wextra -Wpedantic -Wmissing-prototypes -Wredundant-decls \
  -Wshadow -Wpointer-arith -O3 -fomit-frame-pointer -z noexecstack
# -Wmissing-prototypes is C-only, so C++ gets its own flag set.
CXXFLAGS += -Wall -Wextra -Wshadow -Wpointer-arith -O3 -fomit-frame-pointer \
  -z noexecstack
NISTFLAGS += -Wno-unused-result -O3 -fomit-frame-pointer
RM = /bin/rm

# ntt.cpp is compiled separately: it uses C++ templates, so gcc cannot build
# it. Everything else stays plain C. Each KYBER_K needs its own object because
# the reference builds all three parameter sets from one source tree.
SOURCES = kem.c indcpa.c polyvec.c poly.c cbd.c reduce.c verify.c
SOURCESKECCAK = $(SOURCES) fips202.c symmetric-shake.c
HEADERS = params.h kem.h indcpa.h polyvec.h poly.h ntt.h cbd.h reduce.h verify.h symmetric.h
HEADERSKECCAK = $(HEADERS) fips202.h

.PHONY: all speed shared clean

all: test speed shared nistkat

test: \
  test/test_kyber512 \
  test/test_kyber768 \
  test/test_kyber1024 \
  test/test_vectors512 \
  test/test_vectors768 \
  test/test_vectors1024 \

speed: \
  test/test_speed512 \
  test/test_speed768 \
  test/test_speed1024 \

shared: \
  lib/libpqcrystals_kyber512_ref.so \
  lib/libpqcrystals_kyber768_ref.so \
  lib/libpqcrystals_kyber1024_ref.so \
  lib/libpqcrystals_fips202_ref.so \

nistkat: \
	nistkat/PQCgenKAT_kem512 \
	nistkat/PQCgenKAT_kem768 \
	nistkat/PQCgenKAT_kem1024 \

# --------------------------------------------------------------------------
# HLS NTT objects -- one per parameter set, plus -fPIC copies for the shared
# libraries.
# --------------------------------------------------------------------------

ntt512.o: ntt.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -DKYBER_K=2 -c ntt.cpp -o $@

ntt768.o: ntt.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -DKYBER_K=3 -c ntt.cpp -o $@

ntt1024.o: ntt.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -DKYBER_K=4 -c ntt.cpp -o $@

ntt512_pic.o: ntt.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -fPIC -DKYBER_K=2 -c ntt.cpp -o $@

ntt768_pic.o: ntt.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -fPIC -DKYBER_K=3 -c ntt.cpp -o $@

ntt1024_pic.o: ntt.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -fPIC -DKYBER_K=4 -c ntt.cpp -o $@

# --------------------------------------------------------------------------
# Shared libraries
# --------------------------------------------------------------------------

lib/libpqcrystals_fips202_ref.so: fips202.c fips202.h
	mkdir -p lib
	$(CC) -shared -fPIC $(CFLAGS) fips202.c -o $@

lib/libpqcrystals_kyber512_ref.so: $(SOURCES) $(HEADERS) symmetric-shake.c ntt512_pic.o
	mkdir -p lib
	$(CC) -shared -fPIC $(CFLAGS) -DKYBER_K=2 $(SOURCES) symmetric-shake.c ntt512_pic.o -o $@ -lstdc++

lib/libpqcrystals_kyber768_ref.so: $(SOURCES) $(HEADERS) symmetric-shake.c ntt768_pic.o
	mkdir -p lib
	$(CC) -shared -fPIC $(CFLAGS) -DKYBER_K=3 $(SOURCES) symmetric-shake.c ntt768_pic.o -o $@ -lstdc++

lib/libpqcrystals_kyber1024_ref.so: $(SOURCES) $(HEADERS) symmetric-shake.c ntt1024_pic.o
	mkdir -p lib
	$(CC) -shared -fPIC $(CFLAGS) -DKYBER_K=4 $(SOURCES) symmetric-shake.c ntt1024_pic.o -o $@ -lstdc++

# --------------------------------------------------------------------------
# Functional tests
# --------------------------------------------------------------------------

test/test_kyber512: $(SOURCESKECCAK) $(HEADERSKECCAK) test/test_kyber.c randombytes.c ntt512.o
	$(CC) $(CFLAGS) -DKYBER_K=2 $(SOURCESKECCAK) randombytes.c test/test_kyber.c ntt512.o -o $@ -lstdc++

test/test_kyber768: $(SOURCESKECCAK) $(HEADERSKECCAK) test/test_kyber.c randombytes.c ntt768.o
	$(CC) $(CFLAGS) -DKYBER_K=3 $(SOURCESKECCAK) randombytes.c test/test_kyber.c ntt768.o -o $@ -lstdc++

test/test_kyber1024: $(SOURCESKECCAK) $(HEADERSKECCAK) test/test_kyber.c randombytes.c ntt1024.o
	$(CC) $(CFLAGS) -DKYBER_K=4 $(SOURCESKECCAK) randombytes.c test/test_kyber.c ntt1024.o -o $@ -lstdc++

test/test_vectors512: $(SOURCESKECCAK) $(HEADERSKECCAK) test/test_vectors.c ntt512.o
	$(CC) $(CFLAGS) -DKYBER_K=2 $(SOURCESKECCAK) test/test_vectors.c ntt512.o -o $@ -lstdc++

test/test_vectors768: $(SOURCESKECCAK) $(HEADERSKECCAK) test/test_vectors.c ntt768.o
	$(CC) $(CFLAGS) -DKYBER_K=3 $(SOURCESKECCAK) test/test_vectors.c ntt768.o -o $@ -lstdc++

test/test_vectors1024: $(SOURCESKECCAK) $(HEADERSKECCAK) test/test_vectors.c ntt1024.o
	$(CC) $(CFLAGS) -DKYBER_K=4 $(SOURCESKECCAK) test/test_vectors.c ntt1024.o -o $@ -lstdc++

# --------------------------------------------------------------------------
# Speed benchmarks
# --------------------------------------------------------------------------

test/test_speed512: $(SOURCESKECCAK) $(HEADERSKECCAK) test/cpucycles.h test/cpucycles.c test/speed_print.h test/speed_print.c test/test_speed.c randombytes.c ntt512.o
	$(CC) $(CFLAGS) -DKYBER_K=2 $(SOURCESKECCAK) randombytes.c test/cpucycles.c test/speed_print.c test/test_speed.c ntt512.o -o $@ -lstdc++

test/test_speed768: $(SOURCESKECCAK) $(HEADERSKECCAK) test/cpucycles.h test/cpucycles.c test/speed_print.h test/speed_print.c test/test_speed.c randombytes.c ntt768.o
	$(CC) $(CFLAGS) -DKYBER_K=3 $(SOURCESKECCAK) randombytes.c test/cpucycles.c test/speed_print.c test/test_speed.c ntt768.o -o $@ -lstdc++

test/test_speed1024: $(SOURCESKECCAK) $(HEADERSKECCAK) test/cpucycles.h test/cpucycles.c test/speed_print.h test/speed_print.c test/test_speed.c randombytes.c ntt1024.o
	$(CC) $(CFLAGS) -DKYBER_K=4 $(SOURCESKECCAK) randombytes.c test/cpucycles.c test/speed_print.c test/test_speed.c ntt1024.o -o $@ -lstdc++

# --------------------------------------------------------------------------
# NIST known-answer tests -- this is the definitive check
# --------------------------------------------------------------------------

nistkat/PQCgenKAT_kem512: $(SOURCESKECCAK) $(HEADERSKECCAK) nistkat/PQCgenKAT_kem.c nistkat/rng.c nistkat/rng.h ntt512.o
	$(CC) $(NISTFLAGS) -DKYBER_K=2 -o $@ $(SOURCESKECCAK) nistkat/rng.c nistkat/PQCgenKAT_kem.c ntt512.o $(LDFLAGS) -lcrypto -lstdc++

nistkat/PQCgenKAT_kem768: $(SOURCESKECCAK) $(HEADERSKECCAK) nistkat/PQCgenKAT_kem.c nistkat/rng.c nistkat/rng.h ntt768.o
	$(CC) $(NISTFLAGS) -DKYBER_K=3 -o $@ $(SOURCESKECCAK) nistkat/rng.c nistkat/PQCgenKAT_kem.c ntt768.o $(LDFLAGS) -lcrypto -lstdc++

nistkat/PQCgenKAT_kem1024: $(SOURCESKECCAK) $(HEADERSKECCAK) nistkat/PQCgenKAT_kem.c nistkat/rng.c nistkat/rng.h ntt1024.o
	$(CC) $(NISTFLAGS) -DKYBER_K=4 -o $@ $(SOURCESKECCAK) nistkat/rng.c nistkat/PQCgenKAT_kem.c ntt1024.o $(LDFLAGS) -lcrypto -lstdc++

clean:
	-$(RM) -f *.gcno *.gcda *.lcov *.o *.so
	-$(RM) -f ntt512.o ntt768.o ntt1024.o
	-$(RM) -f ntt512_pic.o ntt768_pic.o ntt1024_pic.o
	-$(RM) -f test/test_kyber512
	-$(RM) -f test/test_kyber768
	-$(RM) -f test/test_kyber1024
	-$(RM) -f test/test_vectors512
	-$(RM) -f test/test_vectors768
	-$(RM) -f test/test_vectors1024
	-$(RM) -f test/test_speed512
	-$(RM) -f test/test_speed768
	-$(RM) -f test/test_speed1024
	-$(RM) -f nistkat/PQCgenKAT_kem512
	-$(RM) -f nistkat/PQCgenKAT_kem768
	-$(RM) -f nistkat/PQCgenKAT_kem1024
	-$(RM) -f nistkat/*.req
	-$(RM) -f nistkat/*.rsp
	-$(RM) -rf lib/