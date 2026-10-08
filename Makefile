CC ?= gcc
CFLAGS ?= -std=c23 -Wall -Wextra -Wpedantic -Werror -O3 -fPIC -Iinclude -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread -lrt -lm

SRCDIR = src
INCDIR = include
TESTDIR = tests
BENCHDIR = benchmarks
BUILDDIR = build
BINDIR = bin
LIBDIR = lib

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

STATIC_LIB = $(LIBDIR)/librbipc.a
SHARED_LIB = $(LIBDIR)/librbipc.so

TEST_SRCS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(patsubst $(TESTDIR)/%.c, $(BINDIR)/%, $(TEST_SRCS))

BENCH_SRCS = $(wildcard $(BENCHDIR)/*.c)
BENCH_BINS = $(patsubst $(BENCHDIR)/%.c, $(BINDIR)/%, $(BENCH_SRCS))

.PHONY: all clean test bench coverage valgrind clang-tidy compile_commands docs

all: $(STATIC_LIB) $(SHARED_LIB) $(TEST_BINS) compile_commands.json

$(BUILDDIR) $(BINDIR) $(LIBDIR):
	mkdir -p $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/rbipc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJS) | $(LIBDIR)
	ar rcs $@ $^

$(SHARED_LIB): $(OBJS) | $(LIBDIR)
	$(CC) -shared $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BINDIR)/%: $(TESTDIR)/%.c $(STATIC_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(BINDIR)/%: $(BENCHDIR)/%.c $(STATIC_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

test: $(STATIC_LIB) $(TEST_BINS)
	@echo "=================================================="
	@echo "           Running Full Test Suite                "
	@echo "=================================================="
	@for t in $(TEST_BINS); do \
		echo "--> Running $$t ..."; \
		$$t || exit 1; \
		echo ""; \
	done
	@echo "=================================================="
	@echo "           All Tests Passed Successfully!         "
	@echo "=================================================="

bench: $(STATIC_LIB) $(BENCH_BINS)
	@echo "=================================================="
	@echo "           Running Benchmark Suite                "
	@echo "=================================================="
	@for b in $(BENCH_BINS); do \
		echo "--> Running $$b ..."; \
		$$b || exit 1; \
		echo ""; \
	done


coverage: CFLAGS = -std=c23 -Wall -Wextra -Wpedantic -Werror -O0 -g --coverage -fPIC -Iinclude -pthread -D_GNU_SOURCE
coverage: LDFLAGS += --coverage
coverage: clean test
	@echo "=================================================="
	@echo "           Generating Code Coverage Report        "
	@echo "=================================================="
	@gcov -b -o $(BUILDDIR) $(SRCS) > /dev/null
	@python3 scripts/coverage_summary.py

valgrind: $(STATIC_LIB) $(BINDIR)/test_unit_lifecycle $(BINDIR)/test_io_basic $(BINDIR)/test_io_abort
	@echo "=================================================="
	@echo "           Running Valgrind Memory Checks         "
	@echo "=================================================="
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(BINDIR)/test_unit_lifecycle
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(BINDIR)/test_io_basic
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(BINDIR)/test_io_abort
	@echo "Valgrind checks passed with 0 errors!"

clang-tidy:
	@echo "=================================================="
	@echo "           Running Clang-Tidy Static Analysis     "
	@echo "=================================================="
	clang-tidy -checks='bugprone-*,clang-analyzer-*,performance-*,-clang-analyzer-optin.performance.Padding,-bugprone-easily-swappable-parameters' $(SRCS) -- -Iinclude -D_GNU_SOURCE
	@echo "Clang-Tidy analysis passed with 0 errors!"

compile_commands.json: Makefile $(SRCS) $(TEST_SRCS) $(BENCH_SRCS)
	@python3 scripts/gen_compile_commands.py

compile_commands: compile_commands.json

docs:
	@which doxygen > /dev/null 2>&1 && doxygen Doxyfile || echo "doxygen is not installed in the environment; install doxygen to generate HTML documentation."

clean:
	rm -rf $(BUILDDIR) $(BINDIR) $(LIBDIR) compile_commands.json *.gcda *.gcno *.gcov docs/html
