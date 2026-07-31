CXX := clang++
CC  := clang

SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)

CXXFLAGS := -std=c++23 -O2 -Wall -Wextra -pthread -isysroot $(SDKROOT) -isystem vendor/termbox2
CFLAGS   := -std=c17 -Wall -Wextra -isysroot $(SDKROOT)
LDFLAGS  := -pthread -isysroot $(SDKROOT)
LDLIBS   := -lcurl

TARGET := build/app

CPP_SOURCES := $(wildcard *.cpp)
C_SOURCES   := $(wildcard *.c) vendor/md4c/src/md4c.c

CPP_OBJECTS := $(patsubst %.cpp,build/%.cpp.o,$(CPP_SOURCES))
C_OBJECTS   := $(patsubst %.c,build/%.c.o,$(C_SOURCES))
OBJECTS     := $(CPP_OBJECTS) $(C_OBJECTS)
DEPENDENCIES := $(OBJECTS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $< -> $@"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling $< -> $@"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

test:
	@sh tests/run.sh

-include $(DEPENDENCIES)

.PHONY: all clean test
