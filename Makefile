# bpp-tree — compile a join-formula into a BPP species tree.
# C11, no dependencies beyond libc/libm.

CC      ?= cc
CSTD     = -std=c11
WARN     = -Wall -Wextra
OPT      = -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT)
LDLIBS   = -lm

# EXTRA_CFLAGS / EXTRA_LDFLAGS are appended after the defaults. The release
# workflow uses them to cross-compile (e.g. make EXTRA_CFLAGS=-arch\ x86_64
# EXTRA_LDFLAGS=-arch\ x86_64). Override BIN for Windows: make BIN=bpp-tree.exe.
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

SRC      = $(wildcard src/*.c)
OBJ      = $(SRC:.c=.o)
BIN      = bpp-tree

DEBUG_FLAGS = $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined \
              -fno-omit-frame-pointer

.PHONY: all debug test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Sanitiser build (single translation unit pass, fresh objects).
debug:
	$(CC) $(DEBUG_FLAGS) -o $(BIN) $(SRC) $(LDLIBS)

# Standalone harness for the network graph (used by the test suite).
GRAPH_TEST_OBJ = src/newick.o src/graph.o src/util.o src/diag.o
tests/graph_roundtrip: tests/graph_roundtrip.c $(GRAPH_TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(GRAPH_TEST_OBJ) $(LDLIBS)

test: $(BIN) tests/graph_roundtrip
	@./tests/run_tests.sh

clean:
	rm -f $(OBJ) $(BIN) tests/graph_roundtrip
