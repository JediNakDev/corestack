CC      := cc
CFLAGS  := -Wall -Wextra -Werror=unused-result -O2 -Iinclude
LDFLAGS := -Llib

# Asked once and reused: both the linker group and the OpenSSL lookup below
# differ between Apple's toolchain and everyone else's.
UNAME_S := $(shell uname -s)
# --start-group/--end-group: static archives resolve left-to-right and GNU ld
# does not re-scan one it already finished with, so any ordering of our own
# archives here is fragile the moment one of them calls into another (e.g.
# libballotclient's transport.c calling into libtetrissh - see git history,
# this line has been "fixed" by reordering twice already). The group makes
# ld keep re-scanning these until nothing new resolves, so their order here
# stops being load-bearing. System libs (ssl/crypto/pthread) stay outside:
# nothing in the group calls back into them in a cycle that needs it.
#
# Apple's linker has no such once-through limitation - it resolves this
# circularity on its own - and does not recognise --start-group/--end-group
# at all, so passing them there is a hard link error, not a no-op. GRP_START/
# GRP_END are therefore empty on Darwin and the real flags everywhere else.
ifeq ($(UNAME_S),Darwin)
GRP_START :=
GRP_END   :=
else
GRP_START := -Wl,--start-group
GRP_END   := -Wl,--end-group
endif
LDLIBS  := $(GRP_START) -ltetrisauth -ltetrissh -lhtttp -lballotclient -lballotbrain -ltetrisdb -ltetrisutil $(GRP_END) -lssl -lcrypto -lpthread
# OpenSSL, wherever the platform keeps it.
#
# Homebrew installs outside the compiler's default search path, so macOS needs
# an explicit -I/-L. A Linux distro package is already on the default path, and
# asking brew for it there prints "make: brew: No such file or directory" and
# yields an empty prefix - which then expands to -I/include -L/lib, two paths
# that are wrong everywhere and merely harmless while the system copy happens
# to be found anyway.
#
# So: only ask brew on Darwin, and only add the flags if the answer is real.
ifeq ($(UNAME_S),Darwin)
OPENSSL := $(shell brew --prefix openssl 2>/dev/null)
endif
ifneq ($(OPENSSL),)
CFLAGS  += -I$(OPENSSL)/include
LDFLAGS += -L$(OPENSSL)/lib
endif

BIN_DIR := bin
LIB_DIR := lib

#
# Every header, as a prerequisite for everything that compiles.
#
# Without this, changing a struct in include/ rebuilds nothing: make only sees
# the .c files. That is not a slow build, it is a WRONG one - two objects
# compiled against different versions of the same struct link cleanly and then
# disagree about the layout at runtime, which is a bug with no compiler error
# and no stack trace pointing at its cause.
#
# Deliberately coarse: any header touches everything. At this size that costs a
# couple of seconds, and it cannot be wrong the way a hand-maintained list of
# per-target dependencies eventually is.
#
HEADERS := $(shell find include src -name '*.h' 2>/dev/null)
ifeq ($(HEADERS),)
$(warning include/ yielded no headers: builds will not react to header edits)
endif

LIBS := $(LIB_DIR)/libtetrisauth.a $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotbrain.a \
        $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a \
        $(LIB_DIR)/libtetrisui.a

# tetrish system programs (sys, ...) compiled as standalone binaries, PA1-style.
TETRISH_LIB_SRCS := $(wildcard src/tetrish/lib/*.c)
SYSPROG_SRCS     := $(wildcard src/tetrish/system_programs/*.c)
SYSPROG_BINS     := $(SYSPROG_SRCS:src/tetrish/system_programs/%.c=$(BIN_DIR)/%)

BINS := tetrish $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(BIN_DIR)/tetrislogd $(BIN_DIR)/tetrisdb $(BIN_DIR)/ballotctl $(BIN_DIR)/ballotu $(SYSPROG_BINS)

.PHONY: all clean dirs
all: dirs $(LIBS) $(BINS)

dirs:
	@mkdir -p $(BIN_DIR) $(LIB_DIR) var/log var/run

# === Libraries ===
LIBTETRISAUTH_SRCS   := $(wildcard src/libtetrisauth/*.c) \
                        $(wildcard src/libtetrisauth/lib/*.c)
LIBTETRISSH_SRCS     := $(wildcard src/libtetrissh/*.c)
LIBHTTTP_SRCS        := $(wildcard src/libhtttp/*.c)
LIBBALLOTBRAIN_SRCS  := $(wildcard src/libballotbrain/*.c)
LIBBALLOTCLIENT_SRCS := $(wildcard src/libballotclient/*.c)
LIBTETRISDB_SRCS     := $(wildcard src/libtetrisdb/*.c) \
                        $(wildcard src/libtetrisdb/pipe/*.c) \
                        $(wildcard src/libtetrisdb/socket/*.c)
LIBTETRISUTIL_SRCS   := $(wildcard src/libtetrisutil/*.c)
LIBTETRISUI_SRCS     := $(wildcard src/libtetrisui/*.c)

LIBTETRISAUTH_OBJS   := $(LIBTETRISAUTH_SRCS:.c=.o)
LIBTETRISSH_OBJS     := $(LIBTETRISSH_SRCS:.c=.o)
LIBHTTTP_OBJS        := $(LIBHTTTP_SRCS:.c=.o)
LIBBALLOTBRAIN_OBJS  := $(LIBBALLOTBRAIN_SRCS:.c=.o)
LIBBALLOTCLIENT_OBJS := $(LIBBALLOTCLIENT_SRCS:.c=.o)
LIBTETRISDB_OBJS     := $(LIBTETRISDB_SRCS:.c=.o)
LIBTETRISUTIL_OBJS   := $(LIBTETRISUTIL_SRCS:.c=.o)
LIBTETRISUI_OBJS     := $(LIBTETRISUI_SRCS:.c=.o)

# Pattern rule: compile .c -> .o
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_DIR)/libtetrisauth.a: $(LIBTETRISAUTH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrissh.a: $(LIBTETRISSH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libhtttp.a: $(LIBHTTTP_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libballotbrain.a: $(LIBBALLOTBRAIN_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libballotclient.a: $(LIBBALLOTCLIENT_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisdb.a: $(LIBTETRISDB_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisutil.a: $(LIBTETRISUTIL_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisui.a: $(LIBTETRISUI_OBJS)
	ar rcs $@ $^

# === Binaries ===
tetrish: $(wildcard src/tetrish/*.c) $(TETRISH_LIB_SRCS) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# Each system program is its own binary, linked with the shared lib sources.
$(BIN_DIR)/%: src/tetrish/system_programs/%.c $(TETRISH_LIB_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil

# Overrides the pattern rule above: dspawn2 and dcheck read .tetrishrc through
# libtetrisutil's rc_get(), so they need the shared utility library.
$(BIN_DIR)/dspawn2: src/tetrish/system_programs/dspawn2.c $(TETRISH_LIB_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) src/tetrish/system_programs/dspawn2.c $(TETRISH_LIB_SRCS) -o $@ $(LDFLAGS) -ltetrisutil

$(BIN_DIR)/dcheck: src/tetrish/system_programs/dcheck.c $(TETRISH_LIB_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) src/tetrish/system_programs/dcheck.c $(TETRISH_LIB_SRCS) -o $@ $(LDFLAGS) -ltetrisutil

# ballotd and bin/ballot_session are two separate programs: ballotd forks and
# execs bin/ballot_session per voter connection (SESSION_BIN in main.c), so
# session.c has its own main() and must not be linked into ballotd - same
# reasoning as tetrisd/session.c in the sibling tetriSH project. Listed
# explicitly rather than wildcarded for exactly that reason.
$(BIN_DIR)/ballotd: src/ballotd/main.c src/ballotd/dispatch.c src/ballotd/control_plane.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/ballot_session: src/ballotd/session.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrislogd: $(wildcard src/tetrislogd/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrisdb: $(wildcard src/tetrisdb/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# ballotctl and ballotu are the only binaries that draw, so -ltetrisui and
# -lncurses are scoped to them rather than added to the global LDLIBS.
$(BIN_DIR)/ballotctl $(BIN_DIR)/ballotu: LDLIBS += -ltetrisui -lncurses

# The real client: src/ballotctl/ballotctl.c only - same reasoning as
# ballotu below (main.c/mock.c/mock.h/screens.c stay on disk, unbuilt).
$(BIN_DIR)/ballotctl: src/ballotctl/ballotctl.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# The real client: src/ballotu/ballotu.c only. main.c/mock.c/mock.h/
# screens.c stay on disk (the old mock demo) but are deliberately excluded
# here rather than wildcarded - both define main(), and screens.c's
# screen_* functions would otherwise collide with ballotu.c's.
$(BIN_DIR)/ballotu: src/ballotu/ballotu.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# === Unit tests (Unity) ===
UNITY_DIR   := external/2026-pa1-50005-6767/tests/unity
TEST_SRCS   := $(wildcard tests/unit/test_*.c)
TEST_BINS   := $(TEST_SRCS:tests/unit/%.c=$(BIN_DIR)/%)
TEST_CFLAGS := $(CFLAGS) -I$(UNITY_DIR) -Itests/unit/support -Itests

# libballotclient reuses symbols from libballotbrain, so it must precede it.
# -lhtttp is for test_codec, which exercises the wire codec directly against
# real htttp_parse/serialize rather than through a seam.
# -ltetrissh -lssl -lcrypto: voter.c's bu_join/bu_submit_vote reference the
# real bcl_send (transport.c) at the object-file level even when a test only
# calls voter.c's pure functions and never a seam-calling one - the archive
# pulls in the whole of voter.o, and now that bcl_send is real (not the old
# no-dependency stub) that drags libtetrissh in too. A test that defines its
# own bcl_send (fake_client_seams.h) keeps transport.o itself out, but these
# libs still need to be on the link line for the tests that do not.
# -ltetrisdb: bb_alloc_id (ballotbrain.c, always linked - bb_create/bb_destroy
# live there too) calls db_socket_* directly. No test needs to fake it (it
# degrades to a fixed id when unreachable), but the symbols still need to
# resolve.
# -ltetrisutil: bc_fold_eligible (admin.c) calls libtetrisutil/playername.c's
# -ltetrisutil: bc_fold_eligible (admin.c) calls libtetrisutil/playername.c's
# user_name_ok/user_name_fold directly (the same fold every real
# username goes through), so libballotclient.a now has an unresolved
# reference into libtetrisutil.a for every test that links it - which, per the
# note above, is all of them.
# --start-group: same archive-ordering fragility as the top-level LDLIBS -
# see that comment. Each test defines the seams it wants to substitute;
# because the libraries are static archives, a seam defined in the test keeps
# the real member out of the binary (see tests/unit/support/fake_*_seams.h).
# $(LDFLAGS), not a bare -L$(LIB_DIR): LDFLAGS carries the Homebrew OpenSSL
# -L as well, and without it -lssl/-lcrypto below resolve on a Linux distro
# package and fail on macOS, where OpenSSL is off the default search path.
TEST_LDLIBS := $(LDFLAGS) $(GRP_START) -lballotclient -lballotbrain -lhtttp -ltetrissh -ltetrisdb -ltetrisutil $(GRP_END) -lssl -lcrypto -lpthread

$(BIN_DIR)/test_%: tests/unit/test_%.c $(wildcard tests/unit/support/*.h) $(UNITY_DIR)/unity.c $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(TEST_CFLAGS) $< $(UNITY_DIR)/unity.c -o $@ $(TEST_LDLIBS)

# test_db is not a Unity test: it brings its own harness and lives in tests/
# rather than tests/unit/, so it needs an explicit rule to beat the pattern
# rule above. It spawns a real PipeRunner child and skips those cases when
# java or the jar is missing, so it stays runnable on a machine without a JVM.
$(BIN_DIR)/test_db: tests/test_db.c src/tetrisdb/runner.c $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) -Itests -Isrc $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisdb -ltetrisutil -lpthread

$(BIN_DIR)/test_auth: tests/test_auth.c src/tetrisdb/runner.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) -Itests -Isrc/libtetrisauth/lib -Isrc/tetrisdb $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/test_jwt: tests/test_jwt.c src/libtetrisauth/lib/token.c src/libtetrisauth/lib/hex.c $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) -Itests -Isrc/libtetrisauth/lib $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil -lcrypto

$(BIN_DIR)/test_rc: tests/test_rc.c src/tetrislogd/config.c $(BIN_DIR)/tetrislogd $(LIB_DIR)/libtetrisauth.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) -Itests -Isrc/libtetrisauth/lib tests/test_rc.c src/tetrislogd/config.c -o $@ $(LDFLAGS) -ltetrisauth -ltetrisdb -ltetrisutil

$(BIN_DIR)/test_tetrisdb: tests/test_tetrisdb.c $(BIN_DIR)/tetrisdb $(HEADERS)
	$(CC) $(CFLAGS) tests/test_tetrisdb.c -o $@ -lpthread

# Same story as test_db, plus it spawns the real bin/tetrislogd over a socket,
# so the daemon is a build prerequisite rather than just a runtime assumption.
$(BIN_DIR)/test_logd: tests/test_logd.c $(LIB_DIR)/libtetrisutil.a $(BIN_DIR)/tetrislogd $(HEADERS)
	$(CC) $(CFLAGS) tests/test_logd.c -o $@ $(LDFLAGS) -ltetrisutil

# Real-process E2E for ballotd: forks the real bin/ballotd, which itself
# forks bin/ballot_session per voter connection, so both are build
# prerequisites rather than just runtime assumptions (same story as
# test_logd needing bin/tetrislogd). Needs libtetrissh/libhtttp/libballot*
# directly for the client-side handshake and codec calls the test makes.
$(BIN_DIR)/test_ballotd: tests/test_ballotd.c src/tetrisdb/runner.c $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(HEADERS)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@ $(LDFLAGS) -lballotclient -lballotbrain -ltetrissh -lhtttp -ltetrisdb -ltetrisutil -lssl -lcrypto -lpthread

# Real-process E2E for the client side of the same picture: drives the real
# bcl_connect/bu_join/bcl_send (src/libballotclient/transport.c) - the same
# calls ballotu.c makes - against a real bin/ballotd, rather than a
# hand-rolled socket harness like test_ballotd.c's.
$(BIN_DIR)/test_client_transport: tests/test_client_transport.c src/tetrisdb/runner.c $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(HEADERS)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@ $(LDFLAGS) -lballotclient -lballotbrain -ltetrissh -lhtttp -ltetrisdb -ltetrisutil -lssl -lcrypto -lpthread

$(BIN_DIR)/test_system_e2e: tests/test_system_e2e.c src/tetrisdb/runner.c $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libtetrisutil.a $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(BIN_DIR)/tetrisdb $(HEADERS)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@ $(LDFLAGS) -lballotclient -lballotbrain -ltetrissh -lhtttp -ltetrisdb -ltetrisutil -lssl -lcrypto -lpthread

.PHONY: test
test: dirs $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd $(BIN_DIR)/test_auth $(BIN_DIR)/test_jwt $(BIN_DIR)/test_tetrisdb $(BIN_DIR)/test_ballotd $(BIN_DIR)/test_client_transport
	@fail=0; \
	for t in $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd $(BIN_DIR)/test_tetrisdb $(BIN_DIR)/test_jwt $(BIN_DIR)/test_auth $(BIN_DIR)/test_ballotd $(BIN_DIR)/test_client_transport; do \
	  echo "== $$t =="; \
	  $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL UNIT TESTS PASSED"

.PHONY: test-ci
test-ci: dirs $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_auth $(BIN_DIR)/test_jwt
	@fail=0; \
	for t in $(TEST_BINS) $(BIN_DIR)/test_jwt; do \
	  echo "== $$t =="; \
	  $$t || fail=1; \
	done; \
	for t in $(BIN_DIR)/test_db $(BIN_DIR)/test_auth; do \
	  echo "== $$t =="; \
	  TETRISH_NO_RUNNER=1 $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL CI TESTS PASSED"

# === Fuzzing (libFuzzer + ASan/UBSan) ===
#
# Robustness testing, per tests/fuzz/FUZZING.md. Separate from `test` on
# purpose: the harnesses under tests/fuzz/ are built by a DIFFERENT compiler
# with a different instrumentation, so mixing them into the normal build would
# make `make test` depend on a Homebrew package.
#
# Apple's clang ships no libFuzzer runtime - `-fsanitize=fuzzer` fails at link
# with "libclang_rt.fuzzer_osx.a not found" - so these targets use Homebrew's
# LLVM and nothing else does. FUZZ_CC is resolved lazily (= not :=) so that a
# machine without it can still run every other target in this file.
FUZZ_CC     = $(shell brew --prefix llvm 2>/dev/null)/bin/clang
FUZZ_DIR    := tests/fuzz
FUZZ_CORPUS := $(FUZZ_DIR)/corpus
FUZZ_REGRESS:= $(FUZZ_DIR)/regress

# Which sanitizers ride along with libFuzzer.
#
# NOT address, on macOS. Homebrew LLVM 19's AddressSanitizer hangs at process
# startup on Darwin 25 - it never returns from __asan::MemoryRangeIsAvailable
# while scanning for a shadow region, so every fuzz binary built with it hangs
# before main and the run looks like a machine that is thinking hard. Fixed
# upstream in LLVM 20, so this reverts to the full set once `brew upgrade llvm`
# lands that; override explicitly to try:
#
#     make fuzz-build FUZZ_SAN=fuzzer,address,undefined
#
# ASan is not lost in the meantime - bin/replay_* below is built by APPLE's
# clang, whose ASan works, and runs the whole corpus under it. See
# tests/fuzz/replay_main.c for why the split lands where it does.
ifeq ($(UNAME_S),Darwin)
FUZZ_SAN ?= fuzzer,undefined
else
FUZZ_SAN ?= fuzzer,address,undefined
endif

# -O1, not -O2: the sanitizers report against optimised code either way, but
# -O1 keeps frame pointers and inlining tame enough that a crash stack names
# the function that actually broke rather than its caller.
# -fno-sanitize-recover=undefined: UBSan defaults to printing and CONTINUING,
# which would let the fuzzer sail past the very bug it just found.
FUZZ_CFLAGS = -g -O1 -Wall -Wextra -Iinclude -I$(FUZZ_DIR) \
              -fsanitize=$(FUZZ_SAN) \
              -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
              -fprofile-instr-generate -fcoverage-mapping

# The replay half: Apple's clang, working ASan, no libFuzzer. Same harnesses,
# same sources under test, a plain main() from replay_main.c instead of the
# mutation engine.
REPLAY_CFLAGS := -g -O1 -Wall -Wextra -Iinclude -I$(FUZZ_DIR) \
                 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
                 -fno-omit-frame-pointer

FUZZ_SRCS   := $(wildcard $(FUZZ_DIR)/fuzz_*.c)
FUZZ_BINS   := $(FUZZ_SRCS:$(FUZZ_DIR)/fuzz_%.c=$(BIN_DIR)/fuzz_%)
REPLAY_BINS := $(FUZZ_SRCS:$(FUZZ_DIR)/fuzz_%.c=$(BIN_DIR)/replay_%)

# Sources under test, per target. Compiled FROM SOURCE rather than linked out
# of lib/*.a because only instrumented code gives the fuzzer coverage feedback
# to steer by - an uninstrumented archive would leave it mutating blind.
FUZZ_UNDER_TEST_fuzz_htttp_request  := src/libhtttp/htttp.c
FUZZ_UNDER_TEST_fuzz_htttp_response := src/libhtttp/htttp.c
FUZZ_UNDER_TEST_fuzz_codec_request  := src/libballotclient/codec.c src/libhtttp/htttp.c
FUZZ_UNDER_TEST_fuzz_codec_response := src/libballotclient/codec.c src/libhtttp/htttp.c
FUZZ_UNDER_TEST_fuzz_jwt_verify     := src/libtetrisauth/jwt.c
FUZZ_UNDER_TEST_fuzz_rows           := src/libtetrisdb/socket/rows.c
FUZZ_UNDER_TEST_fuzz_rc_line        := src/libtetrisutil/rc.c
FUZZ_UNDER_TEST_fuzz_rc_bind        := src/libtetrisutil/rc.c
FUZZ_UNDER_TEST_fuzz_playername     := src/libtetrisutil/name.c
# ctl_frame.h is header-only (static inline both ends share), so the harness
# IS the translation unit under test.
FUZZ_UNDER_TEST_fuzz_ctl_frame      :=

# Everything else the target needs to link, uninstrumented. codec.c calls
# bb_state_str, which lives in libballotbrain and drags the daemon's whole
# dependency chain behind it; the alternative was a hand-written stub of a
# real function, which is a second copy that can drift from the first.
FUZZ_LDLIBS_fuzz_codec_request  := -L$(LIB_DIR) -lballotbrain -ltetrisdb -ltetrisutil -lpthread
FUZZ_LDLIBS_fuzz_codec_response := $(FUZZ_LDLIBS_fuzz_codec_request)
FUZZ_LDLIBS_fuzz_jwt_verify     := -lcrypto

# OpenSSL's -I/-L ride in through CFLAGS/LDFLAGS above, which the fuzz build
# does not use (different compiler, different sanitizers), so the two flags it
# does need are re-derived here from the same $(OPENSSL) answer.
ifneq ($(OPENSSL),)
FUZZ_SSL_CFLAGS  := -I$(OPENSSL)/include
FUZZ_SSL_LDFLAGS := -L$(OPENSSL)/lib
endif

# Every source under src/, as a prerequisite - the same coarse rule $(HEADERS)
# follows above, and for a sharper reason here. A fuzz binary compiles its
# code under test IN, so a fix to codec.c that make does not notice leaves a
# binary that still reproduces the bug that was just fixed, which reads as
# "the fix did not work" rather than as a stale build.
#
# The Makefile is a prerequisite too: which sanitizers a fuzz binary carries is
# decided HERE, not in the source, so changing FUZZ_SAN with no source edit
# would otherwise leave every stale binary in place - and a stale ASan build on
# macOS does not fail, it hangs, which reads as a slow fuzzer rather than as a
# build that should have been redone.
FUZZ_ALL_SRCS := $(shell find src -name '*.c' 2>/dev/null)

$(BIN_DIR)/fuzz_%: $(FUZZ_DIR)/fuzz_%.c $(FUZZ_DIR)/fuzz_support.h $(HEADERS) $(FUZZ_ALL_SRCS) Makefile
	@mkdir -p $(BIN_DIR)
	@test -x "$(FUZZ_CC)" || { \
	  echo "fuzzing needs Homebrew LLVM (Apple clang has no libFuzzer runtime):"; \
	  echo "  brew install llvm"; exit 1; }
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SSL_CFLAGS) $< \
	  $(FUZZ_UNDER_TEST_fuzz_$*) -o $@ $(FUZZ_SSL_LDFLAGS) $(FUZZ_LDLIBS_fuzz_$*)

$(BIN_DIR)/replay_%: $(FUZZ_DIR)/fuzz_%.c $(FUZZ_DIR)/replay_main.c $(FUZZ_DIR)/fuzz_support.h $(HEADERS) $(FUZZ_ALL_SRCS) Makefile
	@mkdir -p $(BIN_DIR)
	$(CC) $(REPLAY_CFLAGS) $(FUZZ_SSL_CFLAGS) $< $(FUZZ_DIR)/replay_main.c \
	  $(FUZZ_UNDER_TEST_fuzz_$*) -o $@ $(FUZZ_SSL_LDFLAGS) $(FUZZ_LDLIBS_fuzz_$*)

.PHONY: fuzz-build
fuzz-build: $(FUZZ_BINS)

.PHONY: replay-build
replay-build: $(REPLAY_BINS)

# The JWT corpus cannot be written by a shell script: a valid HS256 token is
# not something a mutator (or a printf) will ever produce, and without one the
# target never gets past "not three segments" into the claim logic.
$(BIN_DIR)/seedgen_jwt: $(FUZZ_DIR)/seedgen_jwt.c src/libtetrisauth/jwt.c $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -I$(FUZZ_DIR) $(FUZZ_DIR)/seedgen_jwt.c src/libtetrisauth/jwt.c \
	  -o $@ $(LDFLAGS) -lcrypto

.PHONY: fuzz-seed
fuzz-seed: $(BIN_DIR)/seedgen_jwt
	@sh $(FUZZ_DIR)/seed.sh $(FUZZ_CORPUS)
	@mkdir -p $(FUZZ_CORPUS)/jwt_verify
	@$(BIN_DIR)/seedgen_jwt $(FUZZ_CORPUS)/jwt_verify
	@echo "seeded $(FUZZ_CORPUS)/jwt_verify"

# Run ONE target interactively:  make fuzz T=fuzz_htttp_request [SECS=300]
# Defaults to a five-minute run, which is a coffee break, not a campaign - see
# fuzz-long for the 24-hour version.
T    ?= fuzz_htttp_request
SECS ?= 300
JOBS ?= 4

# Dictionary per target: the grammar's vocabulary, so mutation spends its time
# on the parser's decisions instead of rediscovering "\r\n".
FUZZ_DICT_fuzz_htttp_request  := $(FUZZ_DIR)/dict/htttp.dict
FUZZ_DICT_fuzz_htttp_response := $(FUZZ_DIR)/dict/htttp.dict
FUZZ_DICT_fuzz_codec_request  := $(FUZZ_DIR)/dict/codec.dict
FUZZ_DICT_fuzz_codec_response := $(FUZZ_DIR)/dict/codec.dict
FUZZ_DICT_fuzz_jwt_verify     := $(FUZZ_DIR)/dict/jwt.dict
FUZZ_DICT_fuzz_rows           := $(FUZZ_DIR)/dict/rows.dict
FUZZ_DICT_fuzz_rc_line        := $(FUZZ_DIR)/dict/rc.dict
FUZZ_DICT_fuzz_rc_bind        := $(FUZZ_DIR)/dict/rc.dict

.PHONY: fuzz
fuzz: $(BIN_DIR)/$(T)
	@mkdir -p $(FUZZ_CORPUS)/$(T:fuzz_%=%) $(FUZZ_DIR)/artifacts
	$(BIN_DIR)/$(T) \
	  $(if $(FUZZ_DICT_$(T)),-dict=$(FUZZ_DICT_$(T))) \
	  -max_total_time=$(SECS) -jobs=$(JOBS) -workers=$(JOBS) \
	  -max_len=65536 -rss_limit_mb=4096 -timeout=25 \
	  -artifact_prefix=$(FUZZ_DIR)/artifacts/ \
	  $(FUZZ_CORPUS)/$(T:fuzz_%=%)

# The long campaign: every target, 24 hours divided between them by default.
# FUZZ_SECS is per target, so `make fuzz-long FUZZ_SECS=3600` is an hour each.
FUZZ_SECS ?= 8640
.PHONY: fuzz-long
fuzz-long: $(FUZZ_BINS)
	@sh $(FUZZ_DIR)/run.sh $(FUZZ_SECS)

# The part that outlives the campaign, and the only fuzz target CI gates on:
# every input the fuzzer ever kept, plus every crash ever filed, replayed
# under ASan + UBSan with no mutation. Seconds, not hours, and deterministic -
# a failure here is a bug that was fixed and came back, or a new bug in code
# that an old input now reaches.
#
# Uses bin/replay_* (Apple clang), NOT bin/fuzz_* - so it needs no Homebrew
# LLVM and runs on the same CI runner as `make test-ci`, and so that the
# corpus gets the memory-safety checking the macOS fuzz build cannot do.
.PHONY: fuzz-regress
fuzz-regress: $(REPLAY_BINS)
	@fail=0; \
	for f in $(REPLAY_BINS); do \
	  name=$$(basename $$f); \
	  short=$${name#replay_}; \
	  dirs=""; \
	  for d in $(FUZZ_REGRESS)/$$short $(FUZZ_CORPUS)/$$short; do \
	    n=$$(find $$d -type f ! -name .gitkeep 2>/dev/null | wc -l | tr -d ' '); \
	    [ "$$n" != "0" ] && dirs="$$dirs $$d"; \
	  done; \
	  if [ -z "$$dirs" ]; then echo "== $$short: no inputs, skipped"; continue; fi; \
	  printf '== %s: ' "$$short"; \
	  $$f $$dirs || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "FUZZ REGRESSIONS FAILED"; exit 1; fi; \
	echo "ALL FUZZ REGRESSIONS PASSED"

# A short mutation run over every target: the per-PR gate, catching anything a
# minute of fuzzing can reach from the committed corpus.
FUZZ_SMOKE_SECS ?= 60
.PHONY: fuzz-smoke
fuzz-smoke: $(FUZZ_BINS)
	@sh $(FUZZ_DIR)/run.sh $(FUZZ_SMOKE_SECS)

# Shrink the corpus to the smallest set of inputs with the same coverage.
#
# A night of fuzzing leaves thousands of files, most of them a byte apart from
# each other, and all of them get replayed by every future `make fuzz-regress`.
# Merging keeps what the corpus KNOWS and discards what it merely accumulated:
# libFuzzer copies over only the inputs that add coverage. Run it after a
# campaign, before committing the corpus.
.PHONY: fuzz-merge
fuzz-merge: $(FUZZ_BINS)
	@for f in $(FUZZ_BINS); do \
	  name=$$(basename $$f); short=$${name#fuzz_}; \
	  c=$(FUZZ_CORPUS)/$$short; \
	  [ -d "$$c" ] || continue; \
	  before=$$(find $$c -type f | wc -l | tr -d ' '); \
	  rm -rf $$c.min && mkdir -p $$c.min; \
	  $$f -merge=1 $$c.min $$c >/dev/null 2>&1; \
	  after=$$(find $$c.min -type f | wc -l | tr -d ' '); \
	  if [ "$$after" = "0" ]; then rm -rf $$c.min; echo "== $$short: merge produced nothing, kept $$before"; continue; fi; \
	  rm -rf $$c && mv $$c.min $$c; \
	  echo "== $$short: $$before -> $$after"; \
	done

# Which lines of the parsers the corpus actually reaches. Uncovered lines are
# either dead code or a missing dictionary token - both worth knowing before
# claiming a target is "fuzzed".
.PHONY: fuzz-cov
fuzz-cov: $(FUZZ_BINS)
	@sh $(FUZZ_DIR)/coverage.sh

.PHONY: fuzz-clean
fuzz-clean:
	rm -f $(FUZZ_BINS) $(BIN_DIR)/seedgen_jwt $(FUZZ_DIR)/*.profraw $(FUZZ_DIR)/*.profdata
	rm -rf $(FUZZ_DIR)/artifacts/* $(FUZZ_DIR)/cov

.PHONY: final-test
final-test:
	$(MAKE) clean
	TETRISH_REQUIRE_RUNNER=1 $(MAKE) test
	$(MAKE) $(BIN_DIR)/test_system_e2e
	$(BIN_DIR)/test_system_e2e

# Build from scratch and drop straight into the shell. `all` alone can leave a
# stale binary behind when a source is removed rather than changed, and the
# shell is the entry point everything else is reached through.
#
# clean and all are sequenced by recursive $(MAKE), not by listing them as
# prerequisites. As prerequisites they are unordered, so under -j the rm can
# land in the middle of the build it was meant to precede.
.PHONY: start
start:
	$(MAKE) clean
	$(MAKE) all
	./tetrish

clean:
	rm -rf $(BIN_DIR)/* $(LIB_DIR)/*.a src/*/*.o src/*/*/*.o
