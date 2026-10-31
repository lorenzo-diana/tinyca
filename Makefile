# TinyCA — see SPEC.md §13 for the target table this mirrors.

CXX       ?= g++
CROSS     ?=
CXXSTD    := -std=c++11
WARN      := -Wall -Wextra
BASEFLAGS := $(CXXSTD) -O2 $(WARN) -pthread
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

.PHONY: all mock test check-portability cross cross-static format check-format clean install

all: $(BUILD_DIR)/tinyca

$(BUILD_DIR)/tinyca: $(OBJS)
	$(CXX) $(BASEFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(BASEFLAGS) $(INCLUDES) -c $< -o $@

mock: $(BUILD_DIR)/mockllm

$(BUILD_DIR)/mockllm: $(MOCK_OBJS)
ifneq ($(strip $(MOCK_OBJS)),)
	$(CXX) $(BASEFLAGS) $(MOCK_OBJS) -o $@
else
	@echo "no tools/mockllm/*.cpp yet (D2) -- skipping"
endif

$(BUILD_DIR)/mockllm_obj/%.o: $(MOCK_SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(BASEFLAGS) $(INCLUDES) -c $< -o $@

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

cross-static:
	$(MAKE) CXX=$(CROSS)g++ BUILD_DIR=build/cross-static BASEFLAGS="$(BASEFLAGS) -static" all

format:
	clang-format -i $(wildcard $(INC_DIR)/*.h) $(SRCS) $(MOCK_SRCS)

check-format:
	clang-format --dry-run --Werror $(wildcard $(INC_DIR)/*.h) $(SRCS) $(MOCK_SRCS)

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -D -m 755 $(BUILD_DIR)/tinyca $(DESTDIR)/usr/local/bin/tinyca
