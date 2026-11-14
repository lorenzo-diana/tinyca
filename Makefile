# TinyCA — see SPEC.md §13 for the target table this mirrors.

CXX       ?= g++
# The aarch64 toolchain prefix. Defaulted rather than left empty: an empty
# prefix means plain g++, so `make cross` on an x86-64 host without the
# toolchain installed quietly produced an x86-64 binary in build/cross/ -- a
# "cross build" that verified nothing. check-arch below is the backstop.
CROSS     ?= aarch64-linux-gnu-
# The Intel/AMD 64-bit toolchain prefix. Empty would mean "whatever g++ is",
# which on the aarch64 board silently produces an aarch64 binary -- the one
# mistake this target exists to prevent, hence a prefix rather than a flag.
X86_CROSS ?= x86_64-linux-gnu-
CXXSTD    := -std=c++11
WARN      := -Wall -Wextra
BASEFLAGS := $(CXXSTD) -O2 $(WARN) -pthread
# -MMD -MP emits a .d per object listing the headers it included, so editing a
# header rebuilds everything that includes it. Without this, a changed struct
# in include/tinyca/ left stale objects behind and the link produced a binary
# whose translation units disagreed on layout.
DEPFLAGS  := -MMD -MP
INCLUDES  := -Iinclude -Ithird_party

BUILD_DIR := build
SRC_DIR   := src
INC_DIR   := include/tinyca

SRCS      := $(wildcard $(SRC_DIR)/*.cpp)
OBJS      := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

MOCK_SRC_DIR := tools/mockllm
MOCK_SRCS    := $(wildcard $(MOCK_SRC_DIR)/*.cpp)
MOCK_OBJS    := $(patsubst $(MOCK_SRC_DIR)/%.cpp,$(BUILD_DIR)/mockllm_obj/%.o,$(MOCK_SRCS))

TEST_DIR  := tests

.PHONY: all mock test check-portability cross cross-static x86_64 x86_64-static check-arch format check-format clean install

all: $(BUILD_DIR)/tinyca

$(BUILD_DIR)/tinyca: $(OBJS)
	$(CXX) $(BASEFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(BASEFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

mock: $(BUILD_DIR)/mockllm

$(BUILD_DIR)/mockllm: $(MOCK_OBJS)
ifneq ($(strip $(MOCK_OBJS)),)
	$(CXX) $(BASEFLAGS) $(MOCK_OBJS) -o $@
else
	@echo "no tools/mockllm/*.cpp yet (D2) -- skipping"
endif

$(BUILD_DIR)/mockllm_obj/%.o: $(MOCK_SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(BASEFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

-include $(OBJS:.o=.d) $(MOCK_OBJS:.o=.d)

test: all mock
	@$(TEST_DIR)/run_tests.sh

# Tier 2: same source, different compiler on the same host. Catches C++14/17
# constructs that gcc's -std flag alone won't always reject outright.
check-portability:
	@echo "=== compiling under -std=c++11 -pedantic-errors ==="
	$(CXX) -std=c++11 -pedantic-errors $(WARN) -Wpedantic $(INCLUDES) -fsyntax-only $(SRCS) $(MOCK_SRCS)
	@echo "=== grep ban list: C++14/17-only constructs ==="
	@! grep -rnE '\bstd::(make_unique|optional|filesystem|string_view|variant|any)\b|\bauto\s*\[' $(INC_DIR) $(SRC_DIR) $(MOCK_SRC_DIR) 2>/dev/null \
		|| { echo "banned C++14/17 construct found above"; exit 1; }
	@echo "check-portability: OK"

# Tier 3: syntax/codegen check for aarch64 only — never deployed (SPEC.md §13).
cross:
	$(MAKE) CXX=$(CROSS)g++ BUILD_DIR=build/cross all
	@$(MAKE) --no-print-directory check-arch BIN=build/cross/tinyca WANT=AArch64

cross-static:
	$(MAKE) CXX=$(CROSS)g++ BUILD_DIR=build/cross-static BASEFLAGS="$(BASEFLAGS) -static" all
	@$(MAKE) --no-print-directory check-arch BIN=build/cross-static/tinyca WANT=AArch64

# Intel/AMD 64-bit build => build/x86_64/tinyca. On an x86-64 host this is the
# same compilation `all` does, named explicitly; on the aarch64 board it is a
# genuine cross-build and needs g++-x86-64-linux-gnu installed. Either way the
# result is checked rather than assumed: a binary of the wrong architecture is
# a failure that otherwise only shows up on the target machine.
x86_64:
	$(MAKE) CXX=$(X86_CROSS)g++ BUILD_DIR=build/x86_64 all
	@$(MAKE) --no-print-directory check-arch BIN=build/x86_64/tinyca WANT=X86-64

x86_64-static:
	$(MAKE) CXX=$(X86_CROSS)g++ BUILD_DIR=build/x86_64-static BASEFLAGS="$(BASEFLAGS) -static" all
	@$(MAKE) --no-print-directory check-arch BIN=build/x86_64-static/tinyca WANT=X86-64

# Reads the ELF header of $(BIN) and fails unless its machine is $(WANT).
# Skipped with a notice where readelf is unavailable, since a missing tool is
# not a build error.
check-arch:
	@if command -v readelf >/dev/null 2>&1; then \
		machine=$$(readelf -h $(BIN) | sed -n 's/.*Machine: *//p'); \
		case "$$machine" in \
			*$(WANT)*) echo "$(BIN): $$machine";; \
			*) echo "$(BIN): built for '$$machine', expected $(WANT)"; exit 1;; \
		esac; \
	else \
		echo "readelf not found -- skipping the architecture check for $(BIN)"; \
	fi

format:
	clang-format -i $(wildcard $(INC_DIR)/*.h) $(SRCS) $(MOCK_SRCS)

check-format:
	clang-format --dry-run --Werror $(wildcard $(INC_DIR)/*.h) $(SRCS) $(MOCK_SRCS)

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -D -m 755 $(BUILD_DIR)/tinyca $(DESTDIR)/usr/local/bin/tinyca
