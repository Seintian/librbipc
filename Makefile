CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -Iinclude -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread -lrt

SRCDIR = src
INCDIR = include
TESTDIR = tests
BUILDDIR = build
BINDIR = bin
LIBDIR = lib

OBJS = $(BUILDDIR)/rbipc.o
STATIC_LIB = $(LIBDIR)/librbipc.a
TEST_BIN = $(BINDIR)/test_harness

.PHONY: all clean test

all: $(STATIC_LIB) $(TEST_BIN)

$(BUILDDIR) $(BINDIR) $(LIBDIR):
	mkdir -p $@

$(BUILDDIR)/rbipc.o: $(SRCDIR)/rbipc.c $(INCDIR)/rbipc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJS) | $(LIBDIR)
	ar rcs $@ $^

$(TEST_BIN): $(TESTDIR)/test_harness.c $(STATIC_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -L$(LIBDIR) -lrbipc $(LDFLAGS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILDDIR) $(BINDIR) $(LIBDIR)
