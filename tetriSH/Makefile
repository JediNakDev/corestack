# tetriSH - the multiplayer Tetris system.
#
# Everything this project shares with ballotbox (the six libtetris* libraries,
# the tetrish shell, tetrisdb and tetrislogd) is built by ../shared.mk out of
# ../core. What is left here is tetriSH's own: libtetrisbrain, tetrisd,
# tetrisctl, tetrisu and the test suites.

# Inputs to ../shared.mk, which reads them as it is included.
#
# -Itests: the suites include "test_output.h" as a bare name, and a handful of
# them are compiled from targets that would otherwise each need the flag.
PROJECT_LIBS      := -ltetrisbrain
PROJECT_LIB_FILES := lib/libtetrisbrain.a
EXTRA_CFLAGS      := -Itests

include ../shared.mk

CLIENT_LDLIBS := -ltetrissh -lhtttp -ltetrisbrain -ltetrisui -ltetrisutil \
                 -lssl -lcrypto -lncurses

CLIENT_LIBS := $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a \
               $(LIB_DIR)/libtetrisbrain.a $(LIB_DIR)/libtetrisutil.a \
               $(LIB_DIR)/libtetrisui.a

# Test binaries live in bin/ alongside the programs, not in a subdirectory:
# tests/run_all.sh's "ci" mode matches suite paths literally ("bin/test_db").
TEST_BIN_DIR := $(BIN_DIR)

TEST_HEADERS := $(shell find tests -name '*.h' 2>/dev/null)

BINS := $(SHARED_BINS) $(BIN_DIR)/tetrisd $(BIN_DIR)/session \
        $(BIN_DIR)/tetrisctl $(BIN_DIR)/tetrisu

.PHONY: all client client-start start
all: dirs rc secret java $(LIBS) $(BINS)

start: clean all
	./tetrish

client: dirs rc tetrish $(BIN_DIR)/tetrisu

client-start: clean client
	./tetrish

# === Libraries ===

LIBTETRISBRAIN_SRCS := $(wildcard src/libtetrisbrain/*.c)
LIBTETRISBRAIN_OBJS := $(call local_objs,$(LIBTETRISBRAIN_SRCS))

$(LIB_DIR)/libtetrisbrain.a: $(LIBTETRISBRAIN_OBJS)
	ar rcs $@ $^

# === Binaries ===

$(BIN_DIR)/tetrisd: src/tetrisd/tetrisd.c src/tetrisd/room.c src/tetrisctl/control_plane.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/session: src/tetrisd/session.c src/tetrisd/history.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# Listed rather than wildcarded: src/tetrisctl also holds control_plane.c, the
# daemon-side half, which links into bin/tetrisd and not here.
#
# src/tetrisd/history.c is the read seam the dashboard's history block and
# BEST column query directly - see include/tetrisd/history.h. It has no
# library of its own (one file, two binaries: bin/session and bin/tetrisctl),
# so it is compiled straight into each, matching src/tetrisctl/control_plane.c's
# own reverse case.
$(BIN_DIR)/tetrisctl: LDLIBS += -ltetrisui -lncurses
$(BIN_DIR)/tetrisctl: src/tetrisctl/tetrisctl.c src/tetrisctl/ctl_client.c \
                      src/tetrisctl/ctl_lifecycle.c src/tetrisctl/ctl_logtail.c \
                      src/tetrisctl/ctl_tui.c src/tetrisd/history.c \
                      $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrisu: $(wildcard src/tetrisu/*.c) $(CLIENT_LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(CLIENT_LDLIBS)

# === Tests ===
#
# Every suite is a standalone binary in bin/, named test_*. tests/run_all.sh
# runs a list of them and aggregates their per-case summary lines; its "ci"
# mode matches suite paths literally ("bin/test_db", ...), which is why the
# test binaries sit directly in bin/ rather than in a subdirectory.
#
# Suites that reach into the shared tree name it through $(SHARED_SRC): the
# database runner, tetrislogd's config parser and libtetrisauth's internals all
# live under ../core/src now, not under this project's src/.

DB_RUNNER_SRC := $(SHARED_SRC)/tetrisdb/runner.c

$(TEST_BIN_DIR)/test_session: tests/test_session.c $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_session.c -o $@ $(LDFLAGS) -ltetrissh -ltetrisutil -lssl -lcrypto

$(TEST_BIN_DIR)/test_htttp: tests/test_htttp.c $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_htttp.c -o $@ $(LDFLAGS) -lhtttp -ltetrisutil

$(TEST_BIN_DIR)/test_logd: tests/test_logd.c src/tetrisd/room.c $(BIN_DIR)/tetrislogd $(BIN_DIR)/tetrisctl $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrisbrain.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_logd.c src/tetrisd/room.c -o $@ $(LDFLAGS) -lhtttp -ltetrisbrain -ltetrisutil

$(TEST_BIN_DIR)/test_db: tests/test_db.c $(DB_RUNNER_SRC) $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisdb -ltetrisutil -lpthread

$(TEST_BIN_DIR)/test_history: tests/test_history.c $(DB_RUNNER_SRC) src/tetrisd/history.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN_DIR)/test_auth: tests/test_auth.c $(DB_RUNNER_SRC) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) -I$(SHARED_SRC)/libtetrisauth/lib -I$(SHARED_SRC)/tetrisdb $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN_DIR)/test_race_cond: tests/test_race_cond.c $(DB_RUNNER_SRC) src/tetrisd/history.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# token.c and hex.c are compiled in directly, and nothing else is: the suite is
# also the architectural assertion that the token layer depends on libcrypto
# alone. An #include of another library in it stops this target linking.
$(TEST_BIN_DIR)/test_token: tests/test_token.c $(SHARED_SRC)/libtetrisauth/lib/token.c $(SHARED_SRC)/libtetrisauth/lib/hex.c $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) -I$(SHARED_SRC)/libtetrisauth/lib $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil -lcrypto

$(TEST_BIN_DIR)/test_rc: tests/test_rc.c $(SHARED_SRC)/tetrislogd/config.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) -I$(SHARED_SRC)/libtetrisauth/lib $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN_DIR)/test_tetrisdb: tests/test_tetrisdb.c $(BIN_DIR)/tetrisdb $(HEADERS)
	$(CC) $(CFLAGS) tests/test_tetrisdb.c -o $@ -lpthread

$(TEST_BIN_DIR)/test_board: tests/test_board.c $(LIBTETRISBRAIN_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil

$(TEST_BIN_DIR)/test_room: tests/test_room.c src/tetrisd/room.c $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil

$(TEST_BIN_DIR)/test_shell: tests/test_shell.c tetrish $(HEADERS)
	$(CC) $(CFLAGS) tests/test_shell.c -o $@

$(TEST_BIN_DIR)/test_ctl: tests/test_ctl.c src/tetrisctl/control_plane.c src/tetrisd/room.c $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -lhtttp -ltetrisutil

# The client half: JSON bodies decoded into structs. No socket, no daemon and
# no terminal, so unlike the GUI harnesses this one runs unattended.
$(TEST_BIN_DIR)/test_ctl_client: tests/test_ctl_client.c src/tetrisctl/ctl_client.c $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -lhtttp -ltetrisutil

# The dashboard's log pane, file-in/lines-out. Production initialization reads
# log_path through libtetrisutil; tests use logtail_init_at() with scratch files.
$(TEST_BIN_DIR)/test_ctl_logtail: tests/test_ctl_logtail.c src/tetrisctl/ctl_logtail.c $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil

$(TEST_BIN_DIR)/test_ctl_saturation: tests/test_ctl_saturation.c $(BIN_DIR)/tetrisd $(BIN_DIR)/session $(BIN_DIR)/tetrisctl $(HEADERS)
	$(CC) $(CFLAGS) tests/test_ctl_saturation.c -o $@

$(TEST_BIN_DIR)/test_load: tests/test_load.c src/tetrisu/net.c $(BIN_DIR)/tetrisd $(BIN_DIR)/session $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) tests/test_load.c src/tetrisu/net.c -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN_DIR)/test_saturation: tests/test_saturation.c src/tetrisu/net.c $(BIN_DIR)/tetrisd $(BIN_DIR)/session $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) tests/test_saturation.c src/tetrisu/net.c -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN_DIR)/test_render_fixtures: tests/test_render_fixtures.c src/tetrisu/render.c $(LIBTETRISBRAIN_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil -lncurses

$(TEST_BIN_DIR)/test_gameui: tests/test_gameui.c src/tetrisu/render.c $(LIBTETRISBRAIN_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil -lncurses

$(TEST_BIN_DIR)/test_render_playthrough: tests/test_render_playthrough.c src/tetrisu/render.c $(LIBTETRISBRAIN_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil -lncurses

$(TEST_BIN_DIR)/test_render_input: tests/test_render_input.c src/tetrisu/render.c $(LIBTETRISBRAIN_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil -lncurses

$(TEST_BIN_DIR)/test_tetrisu_terminal: tests/test_tetrisu_terminal.c $(BIN_DIR)/tetrisu $(LIB_DIR)/libtetrisui.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_tetrisu_terminal.c -o $@ $(LDFLAGS) -ltetrisui -lncurses

$(TEST_BIN_DIR)/test_client: tests/test_client.c src/tetrisu/net.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

UNIT_TEST_BINS := $(TEST_BIN_DIR)/test_board $(TEST_BIN_DIR)/test_htttp \
	$(TEST_BIN_DIR)/test_token $(TEST_BIN_DIR)/test_ctl_client \
	$(TEST_BIN_DIR)/test_ctl_logtail

INTEGRATION_TEST_BINS := $(TEST_BIN_DIR)/test_session $(TEST_BIN_DIR)/test_logd \
	$(TEST_BIN_DIR)/test_db $(TEST_BIN_DIR)/test_auth \
	$(TEST_BIN_DIR)/test_tetrisdb $(TEST_BIN_DIR)/test_room $(TEST_BIN_DIR)/test_ctl \
	$(TEST_BIN_DIR)/test_history $(TEST_BIN_DIR)/test_rc

E2E_TEST_BINS := $(TEST_BIN_DIR)/test_shell $(TEST_BIN_DIR)/test_tetrisu_terminal

STRESS_TEST_BINS := $(TEST_BIN_DIR)/test_ctl_saturation $(TEST_BIN_DIR)/test_load \
	$(TEST_BIN_DIR)/test_race_cond

UI_TEST_BINS := $(TEST_BIN_DIR)/test_render_fixtures $(TEST_BIN_DIR)/test_gameui \
	$(TEST_BIN_DIR)/test_render_playthrough $(TEST_BIN_DIR)/test_render_input

# Driven by hand rather than by run_all.sh: the render harnesses want a
# terminal, and test_client wants an address and a mode on argv, which the
# runner has no way to pass.
MANUAL_TEST_BINS := $(UI_TEST_BINS) $(TEST_BIN_DIR)/test_client

# Deliberately outside STRESS_TEST_BINS: it sweeps four command rates back to
# back, so run_all.sh's stress and ci modes would pay for a measurement run
# nobody asked for. Driven by `make test-saturation`.
MEASUREMENT_TEST_BINS := $(TEST_BIN_DIR)/test_saturation

FAST_TEST_BINS := $(UNIT_TEST_BINS) $(INTEGRATION_TEST_BINS) $(E2E_TEST_BINS)
TEST_BINS := $(FAST_TEST_BINS) $(STRESS_TEST_BINS) $(MANUAL_TEST_BINS) \
	$(MEASUREMENT_TEST_BINS)

$(TEST_BINS): $(TEST_HEADERS)

.PHONY: gui test-ui
gui test-ui: dirs $(UI_TEST_BINS)

.PHONY: test test-fast test-unit test-integration test-e2e test-stress \
	test-full test-client test-manual test-sanitizer test-stress-sanitizer \
	test-stress-leaks test-endurance test-saturation test-ci

# The suites that drive the real simpledb jar are skipped by default: the
# runner binds its socket but never emits <<READY>>, so every one of them
# fails on the handshake rather than on anything the C side does. Each suite
# prints a SKIP line for the group, so the omission stays visible.
#
# Run them with `make test RUNNER=1` once the jar and the local JDK agree.
RUNNER ?= 0
ifeq ($(RUNNER),0)
export TETRISH_NO_RUNNER := 1
endif

test: test-fast

test-fast: dirs $(FAST_TEST_BINS)
	./tests/run_all.sh fast $(UNIT_TEST_BINS) $(INTEGRATION_TEST_BINS) \
		$(E2E_TEST_BINS)

test-unit: dirs $(UNIT_TEST_BINS)
	./tests/run_all.sh fast $(UNIT_TEST_BINS)

test-integration: dirs $(INTEGRATION_TEST_BINS)
	./tests/run_all.sh fast $(INTEGRATION_TEST_BINS)

test-e2e: dirs $(E2E_TEST_BINS)
	./tests/run_all.sh fast $(E2E_TEST_BINS)

test-stress: dirs $(STRESS_TEST_BINS)
	LOAD_IN_S=$${LOAD_IN_S:-10} ./tests/run_all.sh full $(STRESS_TEST_BINS)

test-full: dirs $(FAST_TEST_BINS) $(STRESS_TEST_BINS)
	LOAD_IN_S=$${LOAD_IN_S:-10} ./tests/run_all.sh full \
		$(UNIT_TEST_BINS) $(INTEGRATION_TEST_BINS) $(E2E_TEST_BINS) \
		$(STRESS_TEST_BINS)

# The one suite run_all.sh cannot drive: --history-frames decodes recorded
# frames with no server behind them, so it stays automated - just not through
# the runner, which passes no arguments.
test-client: dirs $(TEST_BIN_DIR)/test_client
	./$(TEST_BIN_DIR)/test_client --history-frames

test-manual: dirs $(MANUAL_TEST_BINS)

test-sanitizer:
	$(MAKE) clean
	$(MAKE) test-fast CFLAGS="$(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

test-stress-sanitizer:
	$(MAKE) clean
	$(MAKE) dirs $(TEST_BIN_DIR)/test_load CFLAGS="$(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"
	SETTLE_MS=$${SETTLE_MS:-15000} LOAD_IN_S=$${LOAD_IN_S:-20} \
		./tests/run_all.sh full $(TEST_BIN_DIR)/test_load

# macOS-only leak detection for the plain (non-sanitizer) test_load stress
# scenarios (issue #117). Apple Clang's ASan has no LeakSanitizer, so this
# covers the gap left by test-stress-sanitizer (#116), which is ASan-based
# and Linux-only. No-ops with a clear message on other platforms rather than
# duplicating that effort.
test-stress-leaks: dirs $(TEST_BIN_DIR)/test_load
ifeq ($(shell uname),Darwin)
	SETTLE_MS=$${SETTLE_MS:-15000} LOAD_IN_S=$${LOAD_IN_S:-20} \
		leaks --atExit -- ./$(TEST_BIN_DIR)/test_load
else
	@echo "test-stress-leaks: 'leaks' is macOS-only; skipping on $(shell uname)."
	@echo "test-stress-leaks: use 'make test-stress-sanitizer' for Linux/ASan leak detection instead."
endif

test-endurance: dirs $(TEST_BIN_DIR)/test_load
	LOAD_IN_S=3600 ./tests/run_all.sh full $(TEST_BIN_DIR)/test_load

# Latency/throughput sweep. LOAD_IN_S is per command rate, and there are four
# of them, so the wall clock is roughly 4 x (LOAD_IN_S + 10s). Writes
# var/saturation/*.csv; the numbers are the deliverable, not the exit status.
test-saturation: dirs $(MEASUREMENT_TEST_BINS)
	LOAD_IN_S=$${LOAD_IN_S:-60} ./$(TEST_BIN_DIR)/test_saturation

test-ci: dirs $(FAST_TEST_BINS) $(STRESS_TEST_BINS)
	./tests/run_all.sh ci $(UNIT_TEST_BINS) $(INTEGRATION_TEST_BINS) \
		$(E2E_TEST_BINS) $(STRESS_TEST_BINS)
