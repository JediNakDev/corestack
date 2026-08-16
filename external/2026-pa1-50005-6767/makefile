CC = gcc

SRC_ROOT = ./source
SRC_DIR = $(SRC_ROOT)/system_programs
LIB_DIR = $(SRC_ROOT)/libs
INC_DIR = ./includes
BIN_DIR = ./bin

LIB_SOURCES = $(wildcard $(LIB_DIR)/*.c)
SYSTEM_PROGRAM_SOURCES = $(wildcard $(SRC_DIR)/*.c)
SYSTEM_PROGRAM_BINS = $(SYSTEM_PROGRAM_SOURCES:$(SRC_DIR)/%.c=$(BIN_DIR)/%)

# Compile every .c file directly under source/
# plus every .c file under source/libs/
# into cseshell.
MAIN_SOURCES = \
	$(wildcard $(SRC_ROOT)/*.c) \
	$(LIB_SOURCES)

MAIN_EXEC = cseshell

CFLAGS = -I$(INC_DIR) -Wall -Wextra

# ----------------------------------------------------------------------
# Test infrastructure
# ----------------------------------------------------------------------

TESTS_DIR = ./tests
UNIT_DIR = $(TESTS_DIR)/unit
INTEGRATION_DIR = $(TESTS_DIR)/integration
UNITY_DIR = $(TESTS_DIR)/unity
UNIT_BIN_DIR = $(UNIT_DIR)/bin

UNIT_SOURCES = $(wildcard $(UNIT_DIR)/test_*.c)
UNIT_BINS = $(UNIT_SOURCES:$(UNIT_DIR)/%.c=$(UNIT_BIN_DIR)/%)

TEST_CFLAGS = -I$(INC_DIR) -I$(UNITY_DIR) -Wall -Wextra

# ----------------------------------------------------------------------
# Build targets
# ----------------------------------------------------------------------

all: $(SYSTEM_PROGRAM_BINS) $(MAIN_EXEC)

$(BIN_DIR)/%: $(SRC_DIR)/%.c $(LIB_SOURCES)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(MAIN_EXEC): $(MAIN_SOURCES)
	$(CC) $(CFLAGS) $^ -o $@

# ----------------------------------------------------------------------
# Test targets
# ----------------------------------------------------------------------

$(UNIT_BIN_DIR)/test_%: $(UNIT_DIR)/test_%.c $(UNITY_DIR)/unity.c $(LIB_SOURCES)
	@mkdir -p $(UNIT_BIN_DIR)
	$(CC) $(TEST_CFLAGS) $^ $(EXTRA_SRC) -o $@

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

integration: $(MAIN_EXEC) $(SYSTEM_PROGRAM_BINS)
	@echo "==> Running integration tests"
	@pass=0; fail=0; \
	for s in $(INTEGRATION_DIR)/*.sh; do \
	  [ -f "$$s" ] || continue; \
	  echo "--- $$s ---"; \
	  if bash $$s; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo ""; \
	echo "Integration tests: $$pass passed, $$fail failed"; \
	test $$fail -eq 0

test: unit integration

ai-unit-tests:
	@if [ -z "$(MODULE)" ]; then \
	  echo "Usage: make ai-unit-tests MODULE=name"; \
	  echo "  Looks for includes/libs/name.h and source/libs/name.c"; \
	  echo "  Generates tests/unit/test_name.c"; \
	  exit 1; \
	fi
	@bash ./scripts/gen_unit_tests.sh $(MODULE)

clean:
	rm -f $(SYSTEM_PROGRAM_BINS) $(MAIN_EXEC)
	rm -rf $(UNIT_BIN_DIR)

run: clean all
	./$(MAIN_EXEC)

.PHONY: all clean unit integration test ai-unit-tests run