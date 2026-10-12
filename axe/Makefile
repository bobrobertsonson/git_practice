CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: axe_wav synth_di engine_test

axe_wav: host/axe_wav.cpp host/common.h dsp/axe.h
	$(CXX) $(CXXFLAGS) -o $@ host/axe_wav.cpp

synth_di: host/synth_di.cpp host/common.h
	$(CXX) $(CXXFLAGS) -o $@ host/synth_di.cpp

engine_test: tests/engine_test.cpp host/common.h dsp/axe.h
	$(CXX) $(CXXFLAGS) -o $@ tests/engine_test.cpp

# Unit tests for the engine (no audio files needed).
check: engine_test
	./engine_test

# Synthetic test DIs + renders of every mode into renders/ (git-ignored).
renders: axe_wav synth_di
	tools/render_all.sh

clean:
	rm -f axe_wav synth_di engine_test

.PHONY: all clean check renders lv2check

# Headless LV2 host (lilv) for the built bundle: make lv2check LV2_BUNDLE_DIR=build/AxeElevator_artefacts/Release/LV2
lv2_host: tests/lv2_host.cpp host/common.h
	$(CXX) $(CXXFLAGS) -o $@ tests/lv2_host.cpp $$(pkg-config --cflags --libs lilv-0)

LV2_BUNDLE_DIR ?= build/AxeElevator_artefacts/Release/LV2
lv2check: lv2_host
	LV2_PATH=$(abspath $(LV2_BUNDLE_DIR)) ./lv2_host test urn:snotch:axeelevator
