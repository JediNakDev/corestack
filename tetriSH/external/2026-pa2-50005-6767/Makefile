# =============================================================================
# Makefile for Secure FTP (C / OpenSSL)
# =============================================================================
# Project layout:
#
#   includes/              Public headers
#   includes/libs/         Shared helper headers, e.g. common.h
#   source/                Client/server implementation files
#   source/libs/           Shared helper implementations, e.g. common.c
#   tests/unit/            C unit tests
#   tests/integration/     Bash integration tests
#   tests/unity/           Unity test framework
#
# Usage:
#   make                Build all
#   make NoSec          Build only ClientWithoutSecurity + ServerWithoutSecurity
#   make AP             Build only AP client + server
#   make CP1            Build only CP1 client + server
#   make CP2            Build only CP2 client + server
#   make unit           Compile and run unit tests
#   make integration    Run integration tests
#   make test           Run unit then integration
#   make clean          Remove binaries and reset directories
# =============================================================================

CC = gcc

SRC_ROOT = ./source
LIB_DIR  = $(SRC_ROOT)/libs
INC_DIR  = ./includes

# ----------------------------------------------------------------------
# Shared library sources
# ----------------------------------------------------------------------
# Every .c file under source/libs/ is treated as reusable shared code.
#
# Example:
#   includes/libs/common.h
#   source/libs/common.c
#
# Source files should include it as:
#   #include "libs/common.h"

LIB_SOURCES = $(wildcard $(LIB_DIR)/*.c)

# ----------------------------------------------------------------------
# Main client/server programs
# ----------------------------------------------------------------------

ALL = ClientWithoutSecurity ServerWithoutSecurity \
      ClientWithSecurityAP ServerWithSecurityAP \
      ClientWithSecurityCP1 ServerWithSecurityCP1 \
      ClientWithSecurityCP2 ServerWithSecurityCP2

# ----------------------------------------------------------------------
# OpenSSL detection
# ----------------------------------------------------------------------

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS: Homebrew OpenSSL is installed under a non-standard prefix.
    OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)

    ifeq ($(OPENSSL_PREFIX),)
        $(error OpenSSL not found. Run: brew install openssl)
    endif

    CFLAGS  = -Wall -Wextra -O2 -I$(INC_DIR) -I$(OPENSSL_PREFIX)/include
    LDFLAGS = -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
else
    # Linux: OpenSSL headers are usually available through the system include path.
    CFLAGS  = -Wall -Wextra -O2 -I$(INC_DIR)
    LDFLAGS = -lssl -lcrypto
endif

.PHONY: all clean AP CP1 CP2 NoSec unit integration test ai-unit-tests

all: $(ALL)

NoSec: ClientWithoutSecurity ServerWithoutSecurity
AP:    ClientWithSecurityAP ServerWithSecurityAP
CP1:   ClientWithSecurityCP1 ServerWithSecurityCP1
CP2:   ClientWithSecurityCP2 ServerWithSecurityCP2

# Generic rule for building each client/server program.
#
# Example:
#   ClientWithoutSecurity depends on:
#     source/ClientWithoutSecurity.c
#     all source/libs/*.c
#
# This keeps reusable helper code in source/libs/.
%: $(SRC_ROOT)/%.c $(LIB_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(sort $^) $(LDFLAGS)

# ----------------------------------------------------------------------
# Test infrastructure
# ----------------------------------------------------------------------

TESTS_DIR       = ./tests
UNIT_DIR        = $(TESTS_DIR)/unit
INTEGRATION_DIR = $(TESTS_DIR)/integration
UNITY_DIR       = $(TESTS_DIR)/unity
UNIT_BIN_DIR    = $(UNIT_DIR)/bin

# Auto-discover every tests/unit/test_*.c file.
UNIT_SOURCES = $(wildcard $(UNIT_DIR)/test_*.c)
UNIT_BINS    = $(UNIT_SOURCES:$(UNIT_DIR)/%.c=$(UNIT_BIN_DIR)/%)

# Unit tests need access to project headers and Unity headers.
TEST_CFLAGS = $(CFLAGS) -I$(UNITY_DIR)

# ----------------------------------------------------------------------
# Unit test build rules
# ----------------------------------------------------------------------
# Naming convention:
#
#   tests/unit/test_common.c
#
# may test either:
#
#   source/common.c
#
# or:
#
#   source/libs/common.c
#
# The rules below try both locations.
#
# Rule 1:
#   If source/<name>.c exists, compile it with the test.
#
# Rule 2:
#   If source/libs/<name>.c exists, compile it with the test.
#
# Rule 3:
#   Fallback for tests that only need shared code from source/libs/.
#
# $(sort $^) removes duplicate source files from the compile command.

$(UNIT_BIN_DIR)/test_%: $(UNIT_DIR)/test_%.c $(UNITY_DIR)/unity.c $(SRC_ROOT)/%.c $(LIB_SOURCES)
	@mkdir -p $(UNIT_BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(sort $^) -o $@ $(LDFLAGS)

$(UNIT_BIN_DIR)/test_%: $(UNIT_DIR)/test_%.c $(UNITY_DIR)/unity.c $(LIB_DIR)/%.c $(LIB_SOURCES)
	@mkdir -p $(UNIT_BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(sort $^) -o $@ $(LDFLAGS)

$(UNIT_BIN_DIR)/test_%: $(UNIT_DIR)/test_%.c $(UNITY_DIR)/unity.c $(LIB_SOURCES)
	@mkdir -p $(UNIT_BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(sort $^) -o $@ $(LDFLAGS)

unit: $(UNIT_BINS)
	@echo "==> Running unit tests"
	@pass=0; fail=0; \
	for t in $(UNIT_BINS); do \
	  echo "--- $$t ---"; \
	  if $$t; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo ""; \
	echo "Unit tests: $$pass passed, $$fail failed"; \
	test $$fail -eq 0

# ----------------------------------------------------------------------
# Integration tests
# ----------------------------------------------------------------------

integration: NoSec
	@echo "==> Running integration tests"
	@pass=0; fail=0; \
	for s in $(INTEGRATION_DIR)/test_*.sh; do \
	  [ -f "$$s" ] || continue; \
	  echo "--- $$s ---"; \
	  if bash $$s; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo ""; \
	echo "Integration tests: $$pass passed, $$fail failed"; \
	test $$fail -eq 0

test: unit integration

# ----------------------------------------------------------------------
# AI-assisted unit test generation
# ----------------------------------------------------------------------

ai-unit-tests:
	@if [ -z "$(MODULE)" ]; then \
	  echo "Usage: make ai-unit-tests MODULE=name"; \
	  echo "  Looks for includes/libs/name.h and source/libs/name.c"; \
	  echo "  Also allows source/name.c for non-library modules"; \
	  echo "  Generates tests/unit/test_name.c"; \
	  exit 1; \
	fi
	@bash ./scripts/gen_unit_tests.sh $(MODULE)

# ----------------------------------------------------------------------
# Clean
# ----------------------------------------------------------------------

clean:
	rm -f $(ALL)
	rm -rf $(UNIT_BIN_DIR)
	chmod +x ./setup.sh
	./setup.sh