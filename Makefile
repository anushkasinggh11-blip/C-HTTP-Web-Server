CPPFLAGS ?=
CXXFLAGS ?= -O2
WARNINGS = -std=c++17 -Wall -Wextra -Wpedantic -pthread

.PHONY: all clean
all: server

server: main.cpp tasks.cpp api.cpp metrics.cpp \
  http.h tasks.h api.h metrics.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNINGS) \
	  main.cpp tasks.cpp api.cpp metrics.cpp -o $@

clean:
	rm -f server
