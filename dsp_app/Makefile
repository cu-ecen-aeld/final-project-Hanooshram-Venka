# Audio Playback Processor Makefile
# This file helps avoid the use of long compiler strings, thus automating the compilation of the Multithreaded DSP application


# --- Compiler Configuration ---
# CXX is the standard variable for the C++ compiler
# '?= Set this variable to this value, only if it hasn't already been set by the environment (Buildroot)
CXX ?= $(CROSS_COMPILE)g++
CXXFLAGS ?= -Wall -Werror -g -std=c++11

# LDFLAGS defines the external shared libraries the linker needs to bundle.
# ALSA for hardware audio, sndfile for WAV parsing, pthreads for multithreading,
# and SoundTouch for the DSP engine.
LDFLAGS ?= -lasound -lsndfile -lpthread -lSoundTouch

# Final name of the binary that will be generated
TARGET = dsp_app

# SRCS lists all the C++ source files required to build the application
SRCS = main.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
