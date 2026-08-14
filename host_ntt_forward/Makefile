CXX ?= g++

TARGET ?= ntt_host
BUILD_DIR ?= build
SOURCE := ntt_host.cpp

# CXX_SYSROOT is the sysroot used by the compiler for the C/C++ runtime.
# XRT_INCLUDE and XRT_LIB_DIR may point at a different, but ABI-compatible,
# board SDK when XRT is not installed in CXX_SYSROOT.
CXX_SYSROOT ?=
XRT_SYSROOT ?= $(CXX_SYSROOT)
XRT_INCLUDE ?= $(XRT_SYSROOT)/usr/include
XRT_LIB_DIR ?= $(XRT_SYSROOT)/usr/lib

CPPFLAGS += -I$(XRT_INCLUDE)
CXXFLAGS += -std=c++17 -O0 -g -Wall -Wextra
LDLIBS += $(XRT_LIB_DIR)/libxrt_coreutil.so
LDLIBS += $(XRT_LIB_DIR)/libuuid.so
LDLIBS += -pthread -lrt
LDFLAGS += -Wl,-rpath-link,$(XRT_LIB_DIR)

ifneq ($(strip $(CXX_SYSROOT)),)
SYSROOT_FLAG := --sysroot=$(CXX_SYSROOT)
endif

.PHONY: all clean check

all: $(BUILD_DIR)/$(TARGET)

check:
	@test -f "$(XRT_INCLUDE)/xrt/xrt_bo.h" || \
		{ echo "Missing XRT headers under XRT_INCLUDE=$(XRT_INCLUDE)"; exit 1; }
	@test -f "$(XRT_LIB_DIR)/libxrt_coreutil.so" || \
		{ echo "Missing libxrt_coreutil.so under XRT_LIB_DIR=$(XRT_LIB_DIR)"; exit 1; }
	@test -f "$(XRT_LIB_DIR)/libuuid.so" || \
		{ echo "Missing libuuid.so under XRT_LIB_DIR=$(XRT_LIB_DIR)"; exit 1; }

$(BUILD_DIR)/$(TARGET): $(SOURCE) Makefile | check
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(SYSROOT_FLAG) $(CPPFLAGS) $(CXXFLAGS) "$<" \
		$(LDFLAGS) $(LDLIBS) -o "$@"

clean:
	$(RM) "$(BUILD_DIR)/$(TARGET)"
