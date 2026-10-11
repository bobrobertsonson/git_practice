CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

rasp_wav: host/rasp_wav.cpp dsp/rasp.h
	$(CXX) $(CXXFLAGS) -o $@ host/rasp_wav.cpp

clean:
	rm -f rasp_wav

.PHONY: clean
