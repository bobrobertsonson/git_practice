CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: rasp_wav live_test

rasp_wav: host/rasp_wav.cpp host/common.h dsp/rasp.h dsp/presets.h
	$(CXX) $(CXXFLAGS) -o $@ host/rasp_wav.cpp

live_test: host/live_test.cpp host/common.h dsp/rasp.h dsp/presets.h
	$(CXX) $(CXXFLAGS) -o $@ host/live_test.cpp

presets_dump: host/presets_dump.cpp dsp/rasp.h dsp/presets.h
	$(CXX) $(CXXFLAGS) -o $@ host/presets_dump.cpp

# Checks that the C++ factory presets and defaults match the playground's.
check: presets_dump
	python3 tools/check_presets.py

clean:
	rm -f rasp_wav live_test presets_dump

.PHONY: all clean check
