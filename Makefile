# PankoBipTalk - a `say` for DECtalk: plain TTS by default, with an
# optional Morse code mode.
#
# Builds the dectalk submodule's engine on first use, then compiles this
# repo's CLI against it and installs the binary into
# dectalk/dist/ so it finds DECtalk.conf and the dictionaries that live
# next to it.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99

DECTALK_DIR   := dectalk
DECTALK_SRC   := $(DECTALK_DIR)/src
OS_VERSION    := $(shell uname -r)
DECTALK_BUILD := $(DECTALK_SRC)/dapi/build/dectalk/$(OS_VERSION)/us/release
DECTALK_DIST  := $(DECTALK_DIR)/dist
DECTALK_LIB   := $(DECTALK_DIST)/lib/libtts_us.so
INCLUDE_DIR   := $(DECTALK_BUILD)/include

TARGET := say

.PHONY: all install clean

all: install

$(DECTALK_LIB): $(DECTALK_SRC)/configure.ac
	@echo "==> Building DECtalk engine (first run, this takes a while)"
	cd $(DECTALK_SRC) \
	  && { [ -x ./configure ] || ./autogen.sh; } \
	  && { [ -f Makefile ] || ./configure; } \
	  && $(MAKE) -j$$(nproc)

$(TARGET): src/main.c src/morse.c src/morse.h $(DECTALK_LIB)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -o $@ src/main.c src/morse.c \
	    -L$(DECTALK_DIST)/lib -ltts -ldl -lm \
	    -Wl,-rpath,'$$ORIGIN/lib'

install: $(TARGET)
	cp -f $(TARGET) $(DECTALK_DIST)/$(TARGET)

clean:
	rm -f $(TARGET)
