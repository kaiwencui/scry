CXX ?= g++
PREFIX ?= $(HOME)/.local
UNAME_S := $(shell uname -s)
PLATFORM_LDFLAGS :=
LTO_FLAGS := -flto=auto
STRIP_FLAGS := --strip-all
ifeq ($(UNAME_S),Darwin)
    CXX := $(if $(filter default,$(origin CXX)),clang++,$(CXX))
    PLATFORM_LDFLAGS += -framework CoreServices
    LTO_FLAGS := -flto=thin
    STRIP_FLAGS :=
endif

CXXFLAGS_BASE := -std=c++23 -Wall -Wextra -Wpedantic -Wno-unused-parameter -pthread -Isrc

ifeq ($(DEBUG),1)
    CXXFLAGS := $(CXXFLAGS_BASE) -O0 -g3 -DSCRY_DEBUG=1 -fsanitize=address,undefined
    LDFLAGS := -pthread -fsanitize=address,undefined $(PLATFORM_LDFLAGS)
else
    ifeq ($(PORTABLE),1)
        ARCH_FLAGS :=
    else
        ARCH_FLAGS := -march=native
    endif
    CXXFLAGS := $(CXXFLAGS_BASE) -O3 $(ARCH_FLAGS) -DNDEBUG $(LTO_FLAGS) -fno-rtti
    LDFLAGS := -pthread -O3 $(ARCH_FLAGS) $(LTO_FLAGS) $(PLATFORM_LDFLAGS)
endif

BUILD_DIR := build
LIB_SRCS := \
	src/gitignore.cpp \
	src/io_uring_reader.cpp \
	src/regex_engine.cpp \
	src/posting.cpp \
	src/symbols.cpp \
	src/indexer.cpp \
	src/delta.cpp \
	src/searcher.cpp \
	src/daemon.cpp \
	src/tui.cpp \
	src/cli.cpp

LIB_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))
MAIN_OBJ := $(BUILD_DIR)/main.o
TEST_OBJ := $(BUILD_DIR)/test_main.o
BENCH_OBJ := $(BUILD_DIR)/bench_engine.o

DEPS := $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BENCH_OBJ:.o=.d)

.PHONY: all clean test bench diff-test strip install uninstall

all: scry scry-bench

scry: $(LIB_OBJS) $(MAIN_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

scry-test: $(LIB_OBJS) $(TEST_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

scry-bench: $(LIB_OBJS) $(BENCH_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_main.o: tests/test_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/bench_engine.o: tests/bench_engine.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test: scry-test
	./scry-test

diff-test: scry
	bash scripts/diff_test.sh

bench: scry scry-bench
	bash scripts/bench.sh

strip: scry
	strip $(STRIP_FLAGS) scry

install: scry
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 scry "$(DESTDIR)$(PREFIX)/bin/scry"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/scry"

clean:
	rm -rf $(BUILD_DIR) scry scry-test scry-bench

-include $(DEPS)
