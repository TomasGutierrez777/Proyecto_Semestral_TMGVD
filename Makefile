CXX ?= g++
CXXFLAGS ?= -O3 -Wall -Wextra -Wpedantic
BUILD_DIR ?= build

all: $(BUILD_DIR)/avance

$(BUILD_DIR)/avance: src/experimentos.cpp src/sketches.hpp src/orden_externo.hpp
	mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -fopenmp $< -o "$@" $(LDFLAGS)

.PHONY: all
