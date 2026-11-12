# Builds the Bird Watch servers for the ARM64 Debian server.
# Uses the same compiler and flags as the Eclipse projects on the VM.
#
#   make            build every server into build/
#   make clean      remove build/

CXX      := aarch64-linux-gnu-g++
CXXFLAGS := -O0 -g3 -Wall -fmessage-length=0
LDFLAGS  := -L/usr/lib/aarch64-linux-gnu
LDLIBS   := -lssl -lcrypto

SERVERS  := socksrvr
BUILD    := build

all: $(SERVERS:%=$(BUILD)/%)

$(BUILD)/%: %/main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
