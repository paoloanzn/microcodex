CXX := clang++
CC  := clang

SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)

CXXFLAGS := -std=c++23 -O2 -Wall -Wextra -pthread -isysroot $(SDKROOT) -isystem vendor/termbox2
CFLAGS   := -std=c17 -Wall -Wextra -isysroot $(SDKROOT)
LDFLAGS  := -pthread -isysroot $(SDKROOT)
LDLIBS   := -lcurl

TARGET := build/app
TEST_TARGET := build/agent_test
RENDER_TEST_TARGET := build/render_test

CPP_SOURCES := $(wildcard *.cpp)
C_SOURCES   := $(wildcard *.c) vendor/md4c/src/md4c.c

CPP_OBJECTS := $(patsubst %.cpp,build/%.cpp.o,$(CPP_SOURCES))
C_OBJECTS   := $(patsubst %.c,build/%.c.o,$(C_SOURCES))
OBJECTS     := $(CPP_OBJECTS) $(C_OBJECTS)
DEPENDENCIES := $(OBJECTS:.o=.d)
TEST_OBJECTS := $(filter-out build/main.cpp.o build/ui.cpp.o,$(CPP_OBJECTS)) $(C_OBJECTS)

all: $(TARGET)

test: $(TEST_TARGET) $(RENDER_TEST_TARGET)
	./$(TEST_TARGET)
	./$(RENDER_TEST_TARGET)

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

$(TEST_TARGET): $(TEST_OBJECTS) build/tests/agent_test.cpp.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(RENDER_TEST_TARGET): $(TEST_OBJECTS) build/tests/render_test.cpp.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf build

-include $(DEPENDENCIES)

.PHONY: all clean test
