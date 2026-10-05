CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -Iinclude -I../include -pthread -D_GNU_SOURCE
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

.PHONY: all clean test compile_commands

all: $(STATIC_LIB) $(TEST_BIN) compile_commands.json

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

compile_commands.json: Makefile
	@echo '[' > $@
	@echo '  {' >> $@
	@echo '    "directory": "'$$(pwd)'",' >> $@
	@echo '    "command": "$(CC) $(CFLAGS) -c src/rbipc.c -o build/rbipc.o",' >> $@
	@echo '    "file": "src/rbipc.c"' >> $@
	@echo '  },' >> $@
	@echo '  {' >> $@
	@echo '    "directory": "'$$(pwd)'",' >> $@
	@echo '    "command": "$(CC) $(CFLAGS) tests/test_harness.c -Llib -lrbipc $(LDFLAGS) -o bin/test_harness",' >> $@
	@echo '    "file": "tests/test_harness.c"' >> $@
	@echo '  }' >> $@
	@echo ']' >> $@

compile_commands: compile_commands.json

clean:
	rm -rf $(BUILDDIR) $(BINDIR) $(LIBDIR) compile_commands.json
