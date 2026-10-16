CC ?= gcc
CFLAGS ?= -std=c23 -Wall -Wextra -Wpedantic -Werror -O3 -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread -lrt -lm

SRCDIR = src
INCDIR = include
TESTDIR = tests
BENCHDIR = benchmarks
BUILDDIR = build
BINDIR = bin
LIBDIR = lib

SRCS = $(wildcard $(SRCDIR)/*/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

STATIC_LIB = $(LIBDIR)/librbipc.a
SHARED_LIB = $(LIBDIR)/librbipc.so

TEST_SRCS = $(wildcard $(TESTDIR)/*/*.c)
TEST_BINS = $(patsubst $(TESTDIR)/%.c, $(BINDIR)/%, $(TEST_SRCS))

BENCH_SRCS = $(wildcard $(BENCHDIR)/*.c)
BENCH_BINS = $(patsubst $(BENCHDIR)/%.c, $(BINDIR)/%, $(BENCH_SRCS))

TOOLDIR = tools
TOOL_SRCS = $(wildcard $(TOOLDIR)/*.c)
TOOL_BINS = $(patsubst $(TOOLDIR)/%.c, $(BINDIR)/%, $(TOOL_SRCS))

.PHONY: all clean test bench bench-laptop bench-laptop-powersave bench-home-pc monitor monitor-live coverage valgrind clang-tidy compile_commands docs tsan asan

all: $(STATIC_LIB) $(SHARED_LIB) $(TEST_BINS) $(TOOL_BINS) compile_commands.json

$(BUILDDIR) $(BINDIR) $(LIBDIR):
	mkdir -p $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/rbipc.h | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJS) | $(LIBDIR)
	ar rcs $@ $^

$(SHARED_LIB): $(OBJS) | $(LIBDIR)
	$(CC) -shared $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BINDIR)/%: $(TESTDIR)/%.c $(STATIC_LIB) | $(BINDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(BINDIR)/%: $(BENCHDIR)/%.c $(BENCHDIR)/bench_common.h $(STATIC_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(BINDIR)/%: $(TOOLDIR)/%.c $(STATIC_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

test: $(STATIC_LIB) $(TEST_BINS)
	@echo "=================================================="
	@echo "           Running Full Test Suite                "
	@echo "=================================================="
	@for t in $(TEST_BINS); do \
		echo "--> Running $$t ..."; \
		$(RUN_TEST) $$t || exit 1; \
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

bench-laptop: $(STATIC_LIB) $(BENCH_BINS)
	@echo "=================================================="
	@echo "      Running Benchmark Suite (Laptop Profile)    "
	@echo "=================================================="
	@for b in $(BENCH_BINS); do \
		echo "--> Running $$b ..."; \
		RBIPC_BENCH_PROFILE=laptop $$b || exit 1; \
		echo ""; \
	done

bench-laptop-powersave: $(STATIC_LIB) $(BENCH_BINS)
	@echo "=================================================="
	@echo " Running Benchmark Suite (Laptop Power-Save Mode) "
	@echo "=================================================="
	@for b in $(BENCH_BINS); do \
		echo "--> Running $$b ..."; \
		RBIPC_BENCH_PROFILE=laptop_powersave $$b || exit 1; \
		echo ""; \
	done

bench-home-pc: $(STATIC_LIB) $(BENCH_BINS)
	@echo "=================================================="
	@echo "    Running Benchmark Suite (Home PC Profile)     "
	@echo "=================================================="
	@for b in $(BENCH_BINS); do \
		echo "--> Running $$b ..."; \
		RBIPC_BENCH_PROFILE=home_pc $$b || exit 1; \
		echo ""; \
	done

RING ?= /rbipc_telemetry_demo
WATCH ?= 0

monitor: $(STATIC_LIB) $(TOOL_BINS)
	@SHM_FILE="/dev/shm/$$(echo '$(RING)' | sed 's|^/||')"; \
	if [ -e "$$SHM_FILE" ]; then \
		echo "Attaching telemetry monitor to active ring $(RING)..."; \
		if [ "$$(echo '$(WATCH) > 0' | bc -l 2>/dev/null || echo 0)" = "1" ]; then \
			$(BINDIR)/rbipc_stat $(RING) --watch $(WATCH) --slots; \
		else \
			$(BINDIR)/rbipc_stat $(RING) --slots; \
		fi; \
	else \
		echo "No active ring $(RING) detected. Launching realistic high-contention MPMC workload..."; \
		$(BINDIR)/rbipc_telemetry_stress $(RING) 2.5 > /dev/null 2>&1 & \
		STRESS_PID=$$!; \
		sleep 0.6; \
		$(BINDIR)/rbipc_stat $(RING) --slots; \
		wait $$STRESS_PID 2>/dev/null || true; \
	fi

monitor-live: $(STATIC_LIB) $(TOOL_BINS)
	@SHM_FILE="/dev/shm/$$(echo '$(RING)' | sed 's|^/||')"; \
	if [ ! -e "$$SHM_FILE" ]; then \
		echo "No active ring $(RING) detected. Launching 10s background stress workload..."; \
		$(BINDIR)/rbipc_telemetry_stress $(RING) 10.0 > /dev/null 2>&1 & \
		sleep 0.5; \
	fi; \
	$(BINDIR)/rbipc_stat $(RING) --watch 0.5 --slots

coverage: CFLAGS = -std=c23 -Wall -Wextra -Wpedantic -Werror -O0 -g --coverage -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE
coverage: LDFLAGS += --coverage
coverage: clean test
	@echo "=================================================="
	@echo "           Generating Code Coverage Report        "
	@echo "=================================================="
	@for s in $(SRCS); do \
		gcov -b -o $$(dirname build/$${s#src/}) $$s > /dev/null; \
	done
	@python3 scripts/coverage_summary.py

valgrind: $(STATIC_LIB) $(BINDIR)/core/test_unit_lifecycle $(BINDIR)/io/test_io_basic $(BINDIR)/io/test_io_abort
	@which valgrind > /dev/null 2>&1 && { \
		echo "=================================================="; \
		echo "           Running Valgrind Memory Checks         "; \
		echo "=================================================="; \
		valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(BINDIR)/core/test_unit_lifecycle && \
		valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(BINDIR)/io/test_io_basic && \
		valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(BINDIR)/io/test_io_abort && \
		echo "Valgrind checks passed with 0 errors!"; \
	} || echo "valgrind is not installed in the environment; install valgrind to run dynamic memory analysis."

clang-tidy:
	@echo "=================================================="
	@echo "           Running Clang-Tidy Static Analysis     "
	@echo "=================================================="
	clang-tidy -checks='bugprone-*,clang-analyzer-*,performance-*,-clang-analyzer-optin.performance.Padding,-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling' $(SRCS) -- -std=c23 -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -D_GNU_SOURCE
	@echo "Clang-Tidy analysis passed with 0 errors!"

compile_commands.json: Makefile $(SRCS) $(TEST_SRCS) $(BENCH_SRCS)
	@python3 scripts/gen_compile_commands.py

compile_commands: compile_commands.json

docs:
	@which doxygen > /dev/null 2>&1 && doxygen Doxyfile && python3 scripts/postprocess_docs.py || echo "doxygen is not installed in the environment; install doxygen to generate HTML documentation."

tsan: CFLAGS = -std=c23 -Wall -Wextra -Wpedantic -Werror -Wno-error=tsan -O1 -g -fsanitize=thread -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE
tsan: LDFLAGS += -fsanitize=thread
tsan: clean
	make test RUN_TEST="setarch $$(uname -m) -R" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

asan: CFLAGS = -std=c23 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean test

clean:
	rm -rf $(BUILDDIR) $(BINDIR) $(LIBDIR) compile_commands.json *.gcda *.gcno *.gcov docs/html
