# Builds uart_tsi against libfesvr from riscv-isa-sim.
#
# RISCV points at the prefix holding include/fesvr and lib/libfesvr.a.  On macOS the
# library may be built for a different CPU than the host (an Intel-prefix Homebrew under
# Rosetta is common); when that happens the matching -arch flag is added automatically
# and the binary runs under Rosetta.  Set ARCHFLAGS explicitly to override.
RISCV ?= /usr/local
CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wno-unused-result

HOST_ARCH := $(shell uname -m)
LIB_ARCHS := $(shell lipo -info $(RISCV)/lib/libfesvr.a 2>/dev/null | sed 's/.*: //')
ifneq ($(LIB_ARCHS),)
  ifeq ($(filter $(HOST_ARCH),$(LIB_ARCHS)),)
    ARCHFLAGS ?= -arch $(firstword $(LIB_ARCHS))
  endif
endif
ARCHFLAGS ?=

SRCS = testchip_uart_tsi.cc testchip_tsi.cc testchip_htif.cc

default: uart_tsi

uart_tsi: $(SRCS) testchip_uart_tsi.h testchip_tsi.h testchip_htif.h
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) -I. -I$(RISCV)/include -L$(RISCV)/lib -o $@ $(SRCS) -lfesvr -lpthread

clean:
	rm -f uart_tsi

.PHONY: default clean
