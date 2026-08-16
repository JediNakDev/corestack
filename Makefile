# corestack - the two projects and the core they share.
#
# This Makefile builds nothing itself. Each project owns its own build (its own
# bin/, lib/, obj/, var/ and .tetrishrc), because each is a running system with
# its own daemons and its own runtime state - so `make` here just runs the same
# target in both, and every target below is a pass-through.
#
# To work on one project, run make inside it:
#
#   cd tetriSH   && make all      # or: make test-fast, make start, ...
#   cd ballotbox && make all      # or: make test, make fuzz-regress, ...
#
# The shared core (core/src, core/include) has no build of its own on purpose.
# It is compiled once per project, with that project's flags and against that
# project's include/ - see shared.mk for why that is not the same object twice.

PROJECTS := tetriSH ballotbox

.PHONY: all clean $(PROJECTS)

all: $(PROJECTS)

$(PROJECTS):
	$(MAKE) -C $@ all

# The Java side is one tree for both projects (db/, reached from each through a
# db symlink), so building it once is building it for everyone.
.PHONY: java
java:
	$(MAKE) -C tetriSH java

# `test` means something different in each project - tetriSH's run_all.sh suites
# against ballotbox's Unity ones - so this runs each project's own idea of it
# rather than inventing a third.
.PHONY: test
test:
	$(MAKE) -C tetriSH test
	$(MAKE) -C ballotbox test

.PHONY: test-ci
test-ci:
	$(MAKE) -C tetriSH test-ci
	$(MAKE) -C ballotbox test-ci

clean:
	@for p in $(PROJECTS); do $(MAKE) -C $$p clean; done
