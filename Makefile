CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2 -g
CPPFLAGS := -Iinclude -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS ?= -pthread

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
TEST := tests/heap_tests

.PHONY: all test clean

all: $(TEST)

$(TEST): $(OBJ) tests/heap_tests.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c include/heap.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(OBJ) tests/*.o $(TEST)
