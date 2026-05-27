# bpp-tree — compile a join-formula into a BPP species tree.
# C11, no dependencies beyond libc/libm.

CC      ?= cc
CSTD     = -std=c11
WARN     = -Wall -Wextra
OPT      = -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT)
LDLIBS   = -lm

SRC      = $(wildcard src/*.c)
OBJ      = $(SRC:.c=.o)
BIN      = bpp-tree

DEBUG_FLAGS = $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined \
              -fno-omit-frame-pointer

.PHONY: all debug test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Sanitiser build (single translation unit pass, fresh objects).
debug:
	$(CC) $(DEBUG_FLAGS) -o $(BIN) $(SRC) $(LDLIBS)

test: $(BIN)
	@./tests/run_tests.sh

clean:
	rm -f $(OBJ) $(BIN)
