# Cross-compiles the DLL with MinGW-w64 (Linux, macOS, or MSYS2), and runs the host-side tests.
#   make                  build build/sekiro_perf.dll
#   make test             build and run the portable unit tests with the host compiler
#   make dist             assemble dist/SekiroPerf/ ready to copy into the game folder
#   make CROSS=x86_64-w64-mingw32-  (default) or CXX_WIN=/path/to/x86_64-w64-mingw32-g++

CROSS   ?= x86_64-w64-mingw32-
CXX_WIN ?= $(CROSS)g++

DEFINES  := -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DWINVER=0x0601
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra $(DEFINES)
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ -s
LDLIBS   := -luser32

SRC := $(wildcard src/*.cpp)
HDR := $(wildcard src/*.h)
DLL := build/sekiro_perf.dll

all: $(DLL)

$(DLL): $(SRC) $(HDR) src/exports.def
	@mkdir -p build
	$(CXX_WIN) $(CXXFLAGS) -o $@ $(SRC) src/exports.def $(LDFLAGS) $(LDLIBS)

test: build/tests
	./build/tests

build/tests: tests/test_sigs.cpp tests/test_fsr.cpp src/pattern.h src/sekiro_sigs.h src/fsr_constants.h
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc -o $@ tests/test_main.cpp tests/test_sigs.cpp tests/test_fsr.cpp

dist: $(DLL)
	rm -rf dist/SekiroPerf
	mkdir -p dist/SekiroPerf
	cp $(DLL) dist/sekiro_perf.ini README.md THIRD_PARTY_NOTICES.md LICENSE dist/SekiroPerf/

clean:
	rm -rf build dist/SekiroPerf

.PHONY: all test dist clean
