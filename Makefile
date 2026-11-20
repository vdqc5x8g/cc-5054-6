CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wno-unused-parameter -pthread
CPPFLAGS := -Ithird_party/openh264/include
LDFLAGS := -pthread
LDLIBS := third_party/openh264/lib/libopenh264.a -lz

SRC := src/main.cpp src/server.cpp src/session.cpp src/encoder_sw.cpp \
       src/encoder_v4l2.cpp src/image.cpp src/png.cpp src/common.cpp

BIN := build/encoder_lab

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf build

.PHONY: all clean
