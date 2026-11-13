CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -Wno-missing-field-initializers -Werror

all: run

test_main: test_main.cpp station_sim.h $(wildcard ../firmware/*/*.h)
	$(CXX) $(CXXFLAGS) -o $@ test_main.cpp

scenarios: scenarios.cpp station_sim.h $(wildcard ../firmware/*/*.h)
	$(CXX) $(CXXFLAGS) -o $@ scenarios.cpp

run: test_main
	./test_main

csv: scenarios
	./scenarios ../docs/data

clean:
	rm -f test_main scenarios

.PHONY: all run csv clean
