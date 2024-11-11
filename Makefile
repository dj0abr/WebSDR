# Compiler and flags
CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wno-unused-result -I/usr/local/include/uWebSockets
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

LDFLAGS = -L$(LIB_PATH) -lpthread -lm -lfftw3f -lsdrplay_api -lz -lliquid /usr/local/lib/uSockets.a

# Source and object files
SRC = kwWebRXpp.cpp SDRHardware.cpp FFTProcessor.cpp WebSocketServer.cpp ClientManager.cpp ClientObject.cpp Tuner.cpp SignalDecoder.cpp NarrowFFT.cpp
OBJ = $(SRC:.cpp=.o)
DEP = $(SRC:.cpp=.d)

# Target executable
TARGET = kwWebRXpp

# Default target
default: $(TARGET)

# Build target executable
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ) $(LDFLAGS)

# Compile source files to object files with dependencies
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

# Include dependency files
-include $(DEP)

# Clean up build files
clean:
	rm -f $(OBJ) $(DEP) $(TARGET)
