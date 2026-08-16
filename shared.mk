# Build rules for the code the projects in this repo share.
#
# Included by tetriSH/Makefile and ballotbox/Makefile as `include ../shared.mk`,
# from a working directory that is the PROJECT root, not this one. Everything
# below therefore names project-local paths bare (src/, include/, bin/) and
# shared ones through $(SHARED_SRC)/$(SHARED_INC), which resolve to
# ../core/src and ../core/include.
#
# What a project Makefile must set BEFORE the include:
#
#   PROJECT_LIBS  the -l flags for its own archives, in the order they resolve
#                 (see LDLIBS below for where they land)
#   EXTRA_CFLAGS  anything else its compiles need (optional)
#
# What it gets back: CC/CFLAGS/LDFLAGS/LDLIBS, the six shared libraries, the
# four shared binaries (tetrish, its system programs, tetrislogd, tetrisdb) and
# the housekeeping targets (dirs, rc, secret, java, clean).
#
# What it must add itself: its own libraries, its own binaries, its own tests.

# Where this file is - i.e. the repo root - taken from make's own record of the
# include rather than hard-coded as "..", so a project one directory deeper
# would still work.
SHARED_ROOT := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
SHARED_SRC  := $(SHARED_ROOT)/core/src
SHARED_INC  := $(SHARED_ROOT)/core/include
SHARED_DB   := $(SHARED_ROOT)/db

CC      := cc

# -Iinclude BEFORE -I$(SHARED_INC): where both name the same header, the
# project's copy wins. That is the seam tetrisdb/provision.h uses - core's
# core/src/tetrisdb/main.c includes it, and each project answers with its own list
# of tables to create.
CFLAGS  := -Wall -Wextra -Werror=unused-result -O2 -Iinclude -I$(SHARED_INC) \
           $(EXTRA_CFLAGS)
LDFLAGS := -Llib

# Asked once and reused: both the linker group and the OpenSSL lookup below
# differ between Apple's toolchain and everyone else's.
UNAME_S := $(shell uname -s)

IS_CLANG := $(shell $(CC) -dM -E - </dev/null | grep -c __clang__)
ifeq ($(IS_CLANG),0)
CFLAGS += -Wno-format-truncation
endif

# --start-group/--end-group: static archives resolve left-to-right and GNU ld
# does not re-scan one it already finished with, so any ordering of our own
# archives here is fragile the moment one of them calls into another (e.g.
# ballotbox's libballotclient calling into libtetrissh - see git history, that
# line was "fixed" by reordering twice already). The group makes ld keep
# re-scanning these until nothing new resolves, so their order stops being
# load-bearing. System libs (ssl/crypto/pthread) stay outside: nothing in the
# group calls back into them in a cycle that needs it.
#
# Apple's linker has no such once-through limitation - it resolves this
# circularity on its own - and does not recognise --start-group/--end-group at
# all, so passing them there is a hard link error, not a no-op. GRP_START/
# GRP_END are therefore empty on Darwin and the real flags everywhere else.
ifeq ($(UNAME_S),Darwin)
GRP_START :=
GRP_END   :=
else
GRP_START := -Wl,--start-group
GRP_END   := -Wl,--end-group
endif

# The link line every daemon in either project uses. $(PROJECT_LIBS) sits in
# the middle because a project's own archives call down into libtetrisdb and
# libtetrisutil and are called from nothing above them - though inside the
# group that placement is a readability choice, not a requirement.
LDLIBS := $(GRP_START) -ltetrisauth -ltetrissh -lhtttp $(PROJECT_LIBS) \
          -ltetrisdb -ltetrisutil $(GRP_END) -lssl -lcrypto -lpthread

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
OBJ_DIR := obj

#
# Every header, as a prerequisite for everything that compiles.
#
# Without this, changing a struct in include/ rebuilds nothing: make only sees
# the .c files. That is not a slow build, it is a WRONG one - two objects
# compiled against different versions of the same struct link cleanly and then
# disagree about the layout at runtime, which is a bug with no compiler error
# and no stack trace pointing at its cause.
#
# Deliberately coarse: any header touches everything, in this project and in
# the shared tree both. At this size that costs a couple of seconds, and it
# cannot be wrong the way a hand-maintained list of per-target dependencies
# eventually is.
#
HEADERS := $(shell find include src $(SHARED_INC) $(SHARED_SRC) -name '*.h' \
                        2>/dev/null)
ifeq ($(HEADERS),)
$(warning include/ yielded no headers: builds will not react to header edits)
endif

# === Object layout ===
#
# Objects go under obj/, NOT next to their source. The shared tree is compiled
# once per project, with that project's CFLAGS and against that project's
# include/ - so tetriSH's tetrisdb/main.o and ballotbox's are different objects
# built from the same file, and writing both to ../core/src/tetrisdb/main.o
# would have each project silently link whichever one was built last.
#
# obj/shared mirrors ../core/src, obj/local mirrors ./src.

$(OBJ_DIR)/shared/%.o: $(SHARED_SRC)/%.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/local/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# $(call shared_objs,<sources under ../core/src>) -> objects under obj/shared
shared_objs = $(patsubst $(SHARED_SRC)/%.c,$(OBJ_DIR)/shared/%.o,$(1))
# $(call local_objs,<sources under ./src>)       -> objects under obj/local
local_objs  = $(patsubst src/%.c,$(OBJ_DIR)/local/%.o,$(1))

# === Shared libraries ===

LIBTETRISAUTH_SRCS := $(wildcard $(SHARED_SRC)/libtetrisauth/*.c) \
                      $(wildcard $(SHARED_SRC)/libtetrisauth/lib/*.c)
LIBTETRISSH_SRCS   := $(wildcard $(SHARED_SRC)/libtetrissh/*.c)
LIBHTTTP_SRCS      := $(wildcard $(SHARED_SRC)/libhtttp/*.c)
LIBTETRISDB_SRCS   := $(wildcard $(SHARED_SRC)/libtetrisdb/*.c) \
                      $(wildcard $(SHARED_SRC)/libtetrisdb/pipe/*.c) \
                      $(wildcard $(SHARED_SRC)/libtetrisdb/socket/*.c)
LIBTETRISUTIL_SRCS := $(wildcard $(SHARED_SRC)/libtetrisutil/*.c)
LIBTETRISUI_SRCS   := $(wildcard $(SHARED_SRC)/libtetrisui/*.c)

LIBTETRISAUTH_OBJS := $(call shared_objs,$(LIBTETRISAUTH_SRCS))
LIBTETRISSH_OBJS   := $(call shared_objs,$(LIBTETRISSH_SRCS))
LIBHTTTP_OBJS      := $(call shared_objs,$(LIBHTTTP_SRCS))
LIBTETRISDB_OBJS   := $(call shared_objs,$(LIBTETRISDB_SRCS))
LIBTETRISUTIL_OBJS := $(call shared_objs,$(LIBTETRISUTIL_SRCS))
LIBTETRISUI_OBJS   := $(call shared_objs,$(LIBTETRISUI_SRCS))

$(LIB_DIR)/libtetrisauth.a: $(LIBTETRISAUTH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrissh.a: $(LIBTETRISSH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libhtttp.a: $(LIBHTTTP_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisdb.a: $(LIBTETRISDB_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisutil.a: $(LIBTETRISUTIL_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisui.a: $(LIBTETRISUI_OBJS)
	ar rcs $@ $^

SHARED_LIBS := $(LIB_DIR)/libtetrisauth.a $(LIB_DIR)/libtetrissh.a \
               $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrisdb.a \
               $(LIB_DIR)/libtetrisutil.a $(LIB_DIR)/libtetrisui.a

# Every archive a daemon in this project links, shared and local both. make
# expands a rule's prerequisites as it reads the rule, so PROJECT_LIB_FILES has
# to be set before the include for the shared binaries below to depend on the
# project's own archives - which is why it is an input to this file rather than
# something the project appends afterwards.
LIBS := $(SHARED_LIBS) $(PROJECT_LIB_FILES)

# === Shared binaries ===

# tetrish system programs (sys, ...) compiled as standalone binaries, PA1-style.
TETRISH_SRCS     := $(wildcard $(SHARED_SRC)/tetrish/*.c)
TETRISH_LIB_SRCS := $(wildcard $(SHARED_SRC)/tetrish/lib/*.c)
SYSPROG_SRCS     := $(wildcard $(SHARED_SRC)/tetrish/system_programs/*.c)
SYSPROG_BINS     := $(SYSPROG_SRCS:$(SHARED_SRC)/tetrish/system_programs/%.c=$(BIN_DIR)/%)

# The shell itself depends on nothing but its own sources - deliberately, it is
# the PA1 program and predates every library here.
tetrish: $(TETRISH_SRCS) $(TETRISH_LIB_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# Each system program is its own binary, linked with the shared shell sources.
# -ltetrisutil covers dspawn2 and dcheck, which read .tetrishrc's
# dspawn_pid_dir through libtetrisutil/rc.h's rc_get() (dspawn2 to write its
# pidfile, dcheck to find it); the rest are self-contained in
# TETRISH_LIB_SRCS and merely ignore it.
$(BIN_DIR)/%: $(SHARED_SRC)/tetrish/system_programs/%.c $(TETRISH_LIB_SRCS) $(LIB_DIR)/libtetrisutil.a $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -ltetrisutil

TETRISLOGD_SRCS := $(wildcard $(SHARED_SRC)/tetrislogd/*.c)
TETRISDB_SRCS   := $(wildcard $(SHARED_SRC)/tetrisdb/*.c)

$(BIN_DIR)/tetrislogd: $(TETRISLOGD_SRCS) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrisdb: $(TETRISDB_SRCS) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

SHARED_BINS := tetrish $(BIN_DIR)/tetrislogd $(BIN_DIR)/tetrisdb $(SYSPROG_BINS)

# === Housekeeping ===

.PHONY: dirs java rc secret clean

dirs:
	@mkdir -p $(BIN_DIR) $(LIB_DIR) $(OBJ_DIR) var/log var/run

# The SimpleDB jar is built once, in the shared db/ tree, and both projects
# reach it through their db symlink - so `make java` in either one produces the
# jar the other one also uses.
java:
	@if [ -n "$$JAVA_HOME" ] && [ ! -x "$$JAVA_HOME/bin/javac" ]; then \
		echo "ignoring JAVA_HOME without javac: $$JAVA_HOME"; \
		unset JAVA_HOME; \
	fi; \
	ant -f $(SHARED_DB)/build.xml

secret:
	@[ -f auth/jwt_secret ] || { mkdir -p auth && umask 077 && \
	  openssl rand -out auth/jwt_secret.tmp 32 && \
	  chmod 600 auth/jwt_secret.tmp && \
	  mv auth/jwt_secret.tmp auth/jwt_secret && \
	  echo "generated auth/jwt_secret (32 bytes, mode 0600)"; }

RC_ROOT := $(subst |,\|,$(subst &,\&,$(subst \,\\,$(CURDIR))))

rc:
	@[ -f .tetrishrc ] || { sed 's|PATH_TO_THIS_PROGRAM|$(RC_ROOT)|g' \
	  sample.tetrishrc > .tetrishrc.tmp && mv .tetrishrc.tmp .tetrishrc && \
	  echo "installed .tetrishrc from sample.tetrishrc (PATH -> $(CURDIR))"; } \
	  || { rm -f .tetrishrc.tmp; exit 1; }

# src/*/*.o and src/*/*/*.o are not where this Makefile puts objects any more -
# they are where it USED to, before the shared tree moved to ../core.
# Left in so one `make clean` on a tree checked out before that change does not
# leave a field of stale objects behind that nothing will ever rebuild.
clean:
	rm -rf tetrish $(BIN_DIR)/* $(LIB_DIR)/*.a $(OBJ_DIR) \
	       src/*/*.o src/*/*/*.o
