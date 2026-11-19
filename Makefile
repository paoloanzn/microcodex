UNAME_S := $(shell uname -s)
PLATFORM_FLAGS :=
PLATFORM_LIBS :=

ifeq ($(UNAME_S),Darwin)
CXX := clang++
CC  := clang
SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
PLATFORM_FLAGS += -isysroot $(SDKROOT)
else ifeq ($(UNAME_S),Linux)
CXX := g++
CC  := gcc
PLATFORM_LIBS += -lcrypto
else
$(error Unsupported operating system: $(UNAME_S))
endif

# Warnings for microcodex's own code: conversions, shadowing, casts and other
# common sources of bugs are reported, so they cannot slip in silently. Missing
# field initializers stay allowed: designated initializers deliberately omit
# members that have defaults. Vendored C sources keep the default -Wall -Wextra.
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-qual \
	-Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2 \
	-Wimplicit-fallthrough -Wextra-semi -Wredundant-decls -Wmissing-declarations \
	-Wzero-as-null-pointer-constant -Wno-missing-field-initializers
# GCC's -Wnull-dereference reports false positives inside inlined libstdc++
# code at -O2 (copying a std::vector of strings), so only clang enables it. The
# clang-tidy lint still applies it to every source.
ifneq ($(findstring Free Software Foundation,$(shell $(CXX) --version 2>/dev/null)),)
WARNINGS += -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
else
WARNINGS += -Wnull-dereference
endif
# `make WERROR=1` treats compiler warnings as errors, as CI does.
WERROR ?= 0
ifeq ($(WERROR),1)
WARNINGS += -Werror
endif

CXXFLAGS := -std=c++23 -O2 $(WARNINGS) -pthread $(PLATFORM_FLAGS) -isystem vendor/termbox2 -isystem vendor/md4c/src
CFLAGS   := -std=c17 -Wall -Wextra $(PLATFORM_FLAGS)
LDFLAGS  := -pthread $(PLATFORM_FLAGS)
LDLIBS   := -lcurl $(PLATFORM_LIBS)

# The linked executable keeps its symbol table for debuggers and profilers.
# build/microcodex, the binary that is tested and released, is a stripped copy
# of it, about a fifth smaller.
TARGET := build/microcodex
UNSTRIPPED_TARGET := build/microcodex.unstripped
STRIP ?= strip

CPP_SOURCES := $(wildcard *.cpp)
HEADERS     := $(wildcard *.h)
C_SOURCES   := $(wildcard *.c) vendor/md4c/src/md4c.c

CPP_OBJECTS := $(patsubst %.cpp,build/%.cpp.o,$(CPP_SOURCES))
C_OBJECTS   := $(patsubst %.c,build/%.c.o,$(C_SOURCES))
OBJECTS     := $(CPP_OBJECTS) $(C_OBJECTS)
DEPENDENCIES := $(OBJECTS:.o=.d)

# clang-tidy needs version 19 or newer to parse libstdc++'s <expected>; CI
# pins clang-tidy-19 and clang-format-18. Override with e.g.
# `make lint CLANG_TIDY=clang-tidy-19`.
CLANG_TIDY   ?= clang-tidy
CLANG_FORMAT ?= clang-format
COMPILE_DB   := build/compile_commands.json
LINT_STAMPS  := $(patsubst %.cpp,build/lint/%.cpp.tidy,$(CPP_SOURCES))

all: $(TARGET) $(COMPILE_DB)

$(UNSTRIPPED_TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TARGET): $(UNSTRIPPED_TARGET)
	$(STRIP) -o $@ $<

build/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $< -> $@"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling $< -> $@"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Compilation database for clang-tidy and clangd. It is rewritten only when its
# content changes, so lint results stay cached between runs.
$(COMPILE_DB): FORCE
	@mkdir -p $(dir $@)
	@{ \
		printf '['; separator=''; \
		for source in $(CPP_SOURCES); do \
			printf '%s\n  {"directory": "%s", "file": "%s", "command": "%s -c %s -o build/%s.o"}' \
				"$$separator" "$(CURDIR)" "$$source" "$(CXX) $(CXXFLAGS)" "$$source" "$$source"; \
			separator=','; \
		done; \
		printf '\n]\n'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

# Lints every source with .clang-tidy, warnings as errors. Headers are checked
# through the sources that include them.
lint: $(LINT_STAMPS)

build/lint/%.cpp.tidy: %.cpp $(HEADERS) .clang-tidy $(COMPILE_DB)
	@mkdir -p $(dir $@)
	@echo "Linting $<"
	@$(CLANG_TIDY) -p build --quiet --warnings-as-errors='*' --extra-arg=-Wno-unknown-warning-option --extra-arg=-Wnull-dereference $<
	@touch $@

format:
	@$(CLANG_FORMAT) -i $(CPP_SOURCES) $(HEADERS)

format-check:
	@$(CLANG_FORMAT) --dry-run --Werror $(CPP_SOURCES) $(HEADERS)

clean:
	rm -rf build

test:
	@sh tests/run.sh

FORCE:

-include $(DEPENDENCIES)

.PHONY: all clean test lint format format-check FORCE
