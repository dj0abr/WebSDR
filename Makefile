# Compiler and flags
CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wno-unused-result -I/usr/local/include/uWebSockets
CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= python3
NODE ?= node
.DEFAULT_GOAL := default
# for use with gdb use these compiler flags
# usage:
# gdb ./kwWebRX
# (gdb) run
# after crash: (gdb) bt
# CXXFLAGS = -g -O0 -std=c++17 -Wall -Wno-unused-result -I/usr/local/include/uWebSockets
# or alternative to search for Memory Errors
# die Option -fsanitize=address gibt im Terminal Infos bei einem Crash aus

# Detect architecture
ARCH := $(shell uname -m)

# Set library path based on architecture
ifeq ($(ARCH), x86_64)
    LIB_PATH = ./lib/x86_64
else ifeq ($(ARCH), aarch64)
    LIB_PATH = ./lib/aarch64
else ifeq ($(ARCH), armhf)
    LIB_PATH = ./lib/armhf
else
    $(error Unsupported architecture: $(ARCH))
endif

SPEEX_BUNDLE = external/speexdsp-1.2.1
SPEEX_ARCHIVE = $(SPEEX_BUNDLE)/lib/linux-$(ARCH)/libspeexdsp.a
ifneq ($(wildcard $(SPEEX_ARCHIVE)),)
    CXXFLAGS += -I$(SPEEX_BUNDLE)/include
    SPEEX_LIBS = $(SPEEX_ARCHIVE)
    SPEEX_DEP = $(SPEEX_ARCHIVE)
else
    # Other architectures use the matching distribution package (libspeexdsp-dev).
    SPEEX_LIBS = -lspeexdsp
endif

LDFLAGS = -L$(LIB_PATH) -lpthread -lm -lfftw3f -lsdrplay_api -lz -lliquid /usr/local/lib/uSockets.a $(SPEEX_LIBS)

# Source and object files
SRC = kwWebRXpp.cpp SDRHardware.cpp FFTProcessor.cpp WebSocketServer.cpp ClientManager.cpp ClientObject.cpp Tuner.cpp SignalDecoder.cpp NoiseReduction.cpp NarrowFFT.cpp ADSB.cpp SDRQUEUE.cpp ADSBstream.cpp ADSB_FFT.cpp FT8Receiver.cpp FT8DecoderProcess.cpp SDRInputResampler.cpp
OBJ = $(SRC:.cpp=.o)
DEP = $(SRC:.cpp=.d)

# Target executable
TARGET = kwWebRXpp
FT8_TARGET = ft8-decoder
FT8_SOURCE_DIR = ft8
FT8_BUILD_DIR = $(FT8_SOURCE_DIR)/build

.PHONY: default all ft8 ft8-build ft8-test ft8-live-test ft8-web-test ft8-url-test ft8-country-test login-test client-diagnostics-test clean

# Default target
default: $(TARGET) $(FT8_TARGET)

all: default

# Keep the decoder's C++/Fortran build in its self-contained source directory.
$(FT8_BUILD_DIR)/CMakeCache.txt: $(FT8_SOURCE_DIR)/CMakeLists.txt $(FT8_SOURCE_DIR)/vendor/wsjtx/sources.cmake
	$(CMAKE) -S $(FT8_SOURCE_DIR) -B $(FT8_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# Let CMake track the Fortran modules, include files and incremental rebuilds.
ft8-build: $(FT8_BUILD_DIR)/CMakeCache.txt
	+$(CMAKE) --build $(FT8_BUILD_DIR)

$(FT8_TARGET): ft8-build
	$(CMAKE) -E copy_if_different $(FT8_BUILD_DIR)/ft8-decoder $@

ft8: $(FT8_TARGET)

ft8-test: $(FT8_TARGET)
	$(CTEST) --test-dir $(FT8_BUILD_DIR) --output-on-failure
	$(PYTHON) $(FT8_SOURCE_DIR)/tools/verify_vendor.py

FT8_LIVE_TEST = $(FT8_BUILD_DIR)/ft8-live-test
$(FT8_LIVE_TEST): ft8/tests/receiver_test.cpp FT8Receiver.cpp FT8Receiver.h FT8DecoderProcess.cpp FT8DecoderProcess.h FT8WebMessage.h SDRStreamInfo.h SDRInputResampler.cpp SDRInputResampler.h SDRQUEUE.cpp SDRQUEUE.h liquid.h | ft8-build
	$(CXX) $(CXXFLAGS) -I. -o $@ ft8/tests/receiver_test.cpp FT8Receiver.cpp FT8DecoderProcess.cpp SDRInputResampler.cpp SDRQUEUE.cpp -L$(LIB_PATH) -lliquid -lpthread -lm

ft8-live-test: $(FT8_TARGET) $(FT8_LIVE_TEST)
	$(PYTHON) ft8/tests/receiver_integration.py $(FT8_LIVE_TEST) $(FT8_BUILD_DIR)/ft8-test-signal

# Exercise the production transport with two WebSocket clients, without SDR hardware.
FT8_WEB_TEST = $(FT8_BUILD_DIR)/ft8-web-test
$(FT8_WEB_TEST): ft8/tests/web_server_test.cpp WebSocketServer.cpp WebSocketServer.h global.h FT8WebMessage.h FT8DecoderProcess.h Callsign.h ClientDiagnostics.h
	mkdir -p $(FT8_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I. -o $@ ft8/tests/web_server_test.cpp WebSocketServer.cpp -lpthread -lm -lz /usr/local/lib/uSockets.a

ft8-web-test: $(FT8_WEB_TEST)
	$(PYTHON) ft8/tests/web_integration.py $(FT8_WEB_TEST)

client-diagnostics-test: $(FT8_WEB_TEST)
	$(PYTHON) tests/client_diagnostics_integration.py $(FT8_WEB_TEST)

# Optional frontend regression checks; Node.js is needed for this target only.
ft8-url-test:
	$(NODE) ft8/tests/websocket_url_test.js

ft8-country-test:
	$(NODE) ft8/tests/country_lookup_test.js

# Check login/counting and band permissions without opening the SDR device.
LOGIN_TEST = $(FT8_BUILD_DIR)/login-test
LOGIN_TEST_SRC = tests/login_test.cpp ClientManager.cpp ClientObject.cpp WebSocketServer.cpp Tuner.cpp SignalDecoder.cpp NoiseReduction.cpp NarrowFFT.cpp FT8Receiver.cpp FT8DecoderProcess.cpp SDRInputResampler.cpp SDRQUEUE.cpp
$(LOGIN_TEST): $(LOGIN_TEST_SRC) $(wildcard *.h) $(SPEEX_DEP)
	mkdir -p $(FT8_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(LOGIN_TEST_SRC) -L$(LIB_PATH) -lliquid -lfftw3f -lpthread -lm -lz /usr/local/lib/uSockets.a $(SPEEX_LIBS)

login-test: $(LOGIN_TEST)
	./$(LOGIN_TEST)
	$(NODE) ft8/tests/websocket_url_test.js

NR_TEST = $(FT8_BUILD_DIR)/noise-reduction-test
$(NR_TEST): tests/noise_reduction_test.cpp NoiseReduction.cpp NoiseReduction.h SignalDecoder.cpp SignalDecoder.h $(SPEEX_DEP)
	mkdir -p $(FT8_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I. -o $@ tests/noise_reduction_test.cpp NoiseReduction.cpp SignalDecoder.cpp -L$(LIB_PATH) -lliquid $(SPEEX_LIBS) -lm

.PHONY: nr-test
nr-test: $(NR_TEST) $(LOGIN_TEST)
	./$(NR_TEST)
	./$(LOGIN_TEST)
	$(NODE) tests/noise_reduction_ui_test.js

# Build target executable
$(TARGET): $(OBJ) $(SPEEX_DEP)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ) $(LDFLAGS)

# Compile source files to object files with dependencies
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

# Include dependency files
-include $(DEP)

# Clean up build files
clean:
	rm -f $(OBJ) $(DEP) $(TARGET) $(FT8_TARGET)
	$(CMAKE) -E rm -rf $(FT8_BUILD_DIR)
