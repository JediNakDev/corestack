# Robustness testing

Fuzzing for BallotBox: what is fuzzed, why those targets, how to run it, and
what to do with a crash. The test *plan* (decision tables, traceability to use
cases) is `../../TEST.md`; the unit test inventory is `../TESTS.md`. This file
is the third leg — the tests nobody wrote by hand.

The difference in one line: a unit test asserts what a function does on the
inputs we thought of, and a fuzz target asserts what a function must *never*
do, on inputs nobody thought of.

## Quick start

```sh
make fuzz-build          # build the fuzz targets (needs: brew install llvm)
make fuzz-seed           # write the seed corpus (once; idempotent)
make fuzz T=fuzz_htttp_request SECS=300     # fuzz one target for 5 minutes
make fuzz-regress        # replay corpus + filed crashes under ASan (the CI gate)
make fuzz-smoke          # 60s of mutation on every target
make fuzz-long           # the campaign: 8640s each = 24h across ten targets
make fuzz-cov            # line coverage of the code under test, from the corpus
```

## Why two compilers

`bin/fuzz_*` and `bin/replay_*` are the same harnesses built twice, and the
split is forced by the toolchains, not chosen:

| | compiler | instrumentation | job |
| --- | --- | --- | --- |
| `bin/fuzz_*` | Homebrew clang | libFuzzer + UBSan | generate inputs, find crashes and hangs |
| `bin/replay_*` | Apple clang | ASan + UBSan | replay the corpus with memory-safety checking |

Apple's clang has no libFuzzer runtime at all (`-fsanitize=fuzzer` fails at
link: `libclang_rt.fuzzer_osx.a not found`). Homebrew's LLVM 19 has libFuzzer
but its AddressSanitizer *hangs at process start* on this macOS — stuck in
`__asan::MemoryRangeIsAvailable` scanning the address space, before `main`,
which looks exactly like a fuzzer working hard. Fixed upstream in LLVM 20, so
after a `brew upgrade llvm` the full set can be tried with:

```sh
make fuzz-build FUZZ_SAN=fuzzer,address,undefined
```

The split is not a workaround with a cost — it is most of the pipeline's value.
Every input the fuzzer keeps lands in `corpus/`, and `make fuzz-regress` re-runs
the whole corpus under a working ASan. A heap overflow that the fuzz binary saw
only as "no crash, interesting coverage" is caught on the next regress run.
It also means **the CI gate needs no Homebrew LLVM**, so it runs on the same
macOS runner as `make test-ci`.

## The targets

Ranked the way they were chosen: how much untrusted input reaches the code, and
how much fixed-size copying it does with it.

| Target | Code under test | Why it is on the list |
| --- | --- | --- |
| `fuzz_htttp_request` | `core/src/libhtttp/htttp.c` | Every decrypted frame from any peer hits this first, before anything has decided the peer is honest. Fills five fixed-size fields from wire bytes. |
| `fuzz_htttp_response` | same | The mirror. Requests flow both directions in HTTTP, so both parsers run on both ends; the status line is code the request target never reaches. |
| `fuzz_codec_request` | `codec.c` + `htttp.c` | The real daemon path: bytes → `htttp_parse_request` → `bcl_decode_request`, exactly what `ballotd` does before any eligibility or lifecycle check. |
| `fuzz_codec_response` | same | The client path. `bcl_decode_response` takes no op, so it will populate any combination of keys — which is what a hostile daemon would send. |
| `fuzz_jwt_verify` | `core/src/libtetrisauth/lib/token.c` | Hand-written field splitting and decimal parsing on a token the caller fully controls, with an auth decision on the other side. Keeps its `jwt` name for its corpus and `regress/` directory. |
| `fuzz_rows` | `core/src/libtetrisdb/socket/rows.c` | Parses SimpleDB's printed output, narration and all. Every credential check reads its salt and digest through it. |
| `fuzz_rc_line` | `core/src/tetrish/lib/rc_parser.c` | Decides whether a line of `.tetrishrc` is a comment, a directive, or **a command the shell will run**. |
| `fuzz_playername` | `core/src/libtetrisutil/name.c` | The allowlist that lets `db_quote`, the credential body split, and the select-reply parser each skip an escaping step. |
| `fuzz_ctl_frame` | `include/libballotclient/ctl_frame.h` | 4-byte length prefix, attacker-chosen, read into a fixed buffer. Driven over a real socketpair so short reads and torn frames actually happen. |

Not yet covered, in rough priority order: `session_recv` (libtetrissh's framing,
needs a handshake fixture), `ballotd`'s dispatcher end to end (a scripted socket
fuzzer, not libFuzzer), and the SimpleDB Java side (Hypothesis or jqf).

## Oracles: what counts as a finding

A crash is the weakest oracle in this directory. Each target also asserts what
its header *promises*, through `FUZZ_CHECK` (see `fuzz_support.h` — it survives
`-DNDEBUG`, unlike `assert`, because these checks are the point of the run):

1. **Memory safety** — ASan/UBSan, on the replay side.
2. **Contract invariants** — a parser returning OK must not leave the caller a
   struct its header says cannot exist: an unterminated fixed-size field, a
   count past the end of its array, a zero-copy slice pointing outside the input.
3. **Round trip** — `decode(encode(decode(x))) == decode(x)`. A failure is two
   peers reading one message differently, which for a ballot means the voter and
   the daemon disagreeing about the vote.
4. **Never-true** — `fuzz_jwt_verify` holds the signing key privately and asserts
   that no fuzzer-invented token verifies. A `JWT_OK` there is an auth bypass,
   the highest-severity finding this directory can produce.
5. **Determinism** — same input twice, same verdict. Divergence means state is
   leaking between calls, which on a server means one request's outcome
   depending on the previous request's.

An oracle that fires on correct behaviour is worse than no oracle, so each
exclusion is documented where it is made. The one so far: `Content-Length` is
excluded from the header round-trip check in `fuzz_htttp_request`, because the
serializer *computes* it from `body_len` — `Content-Length: 0` with no body
legitimately does not survive. That was found by the seed corpus on the first
run, which is the cheapest possible time to find it.

## Layout

```
tests/fuzz/
  fuzz_*.c            one target each; LLVMFuzzerTestOneInput + its oracles
  fuzz_support.h      FUZZ_CHECK, the C-string and slice-containment predicates
  replay_main.c       the standalone main() for bin/replay_* (see "two compilers")
  jwt_fuzz_secret.h   signing key + fixed clock, shared by the target and seedgen
  seedgen_jwt.c       mints the token corpus (a valid HMAC token cannot be printf'd)
  fuzz_rc_bind.c      NOT BUILT - targets rc_bind(), which this repo lacks
  seed.sh             writes every other seed corpus; idempotent
  run.sh              runs every target for N seconds, files crashes, summarises
  coverage.sh         replays the corpus under llvm-cov, reports line coverage
  dict/*.dict         grammar tokens per target
  corpus/<target>/    seeds (committed) + whatever the fuzzer keeps (not committed)
  regress/<target>/   minimised crashes, committed. The CI gate replays these.
  artifacts/          libFuzzer's crash dumps, not committed (see .gitignore)
```

## Baseline (first run, 60s per target)

Recorded so the next campaign has something to compare against. Coverage is of
the code under test only, measured by replaying the committed corpus — not by
the fuzzer's own instrumentation counters, which flatter the number.

| Target | execs/sec | corpus (merged) | line coverage of code under test |
| --- | ---: | ---: | ---: |
| `fuzz_playername` | ~1.9 M | 32 | 100% |
| `fuzz_rc_line` | ~1.6 M | 50 | 23% of rc_parser.c |
| `fuzz_htttp_request` | ~140 k | 150 | 64% of htttp.c |
| `fuzz_htttp_response` | ~140 k | 138 | 73% of htttp.c |
| `fuzz_codec_request` | ~50 k | 228 | 48% codec.c / 66% htttp.c |
| `fuzz_codec_response` | ~40 k | 207 | 50% codec.c / 61% htttp.c |
| `fuzz_ctl_frame` | ~14 k | 12 | socket-bound; 83% branch |
| `fuzz_jwt_verify` | ~8 k | 26 | 79% of token.c |
| `fuzz_rows` | ~7 k | 73 | 96% of rows.c |

Six bugs in the first two hours, all filed in `regress/` and fixed:

| Found by | Bug | Impact |
| --- | --- | --- |
| `fuzz_htttp_request` | `copy_token` accepted a NUL byte inside a header name — `"\0Cert-Name: alice"` parses, and every `htttp_header_get("Cert-Name")` misses it | Header smuggling; also applied to method and path |
| `fuzz_htttp_response` | A message with exactly `HTTTP_MAX_HEADERS` headers serialized OK, then the generated `Date`/`Content-Length` pushed the wire past the parse bound — a frame this library emits and cannot read | Sender sees success, peer sees a 400 |
| `fuzz_codec_request` | `body_append` let a text field carrying `\n`/`\r` become extra body lines: title `"Budget\neligible=mallory"` injects an `eligible=` line into CREATE | Field injection into the ballot protocol |
| `fuzz_codec_response` | `hash_count=`/`tally_count=` taken off the wire by `atoi` while the row writers stop at the array bound — a struct announcing 78 entries in a 64-entry array | A hostile daemon picks how far past the array its client reads |
| `fuzz_codec_request`, `fuzz_codec_response` | `body_for_each` computed `NULL + 0` on a bodyless message | UB on any message without a body |
| `fuzz_rows` | `block_count` accumulated the trailer row count with no overflow guard | Signed overflow, UB |

Two of those are in `libhtttp`, which `DESIGN.md` keeps byte-identical with
tetriSH — **copy both fixes to that repository**, or the `diff` between the two
trees stops answering "has this landed on the other side yet".

And three defects in the harness itself, which is the other half of the work:

| Where | What was wrong |
| --- | --- |
| `fuzz_htttp_request` | `Content-Length` held to a round-trip survival property the serializer cannot meet — it computes that header |
| `fuzz_htttp_request` | Duplicate keys counted with `strcmp` against a case-insensitive lookup: `"B: o"` and `"b: "` are one key to `htttp_header_get`, two to the oracle |
| `fuzz_support.h` | `FUZZ_CHECK` used `__builtin_trap()`. libFuzzer installs no SIGTRAP handler, so **the crashing input was never written to disk** — four findings in one run existed only as scrollback. Now `abort()` |

The three cheap targets (`playername`, `rc_line`, `rows`) are at or near their
coverage ceiling already; the codec pair has the most room left, which is where
a long campaign will pay.

## When the fuzzer finds something

`run.sh` does the filing automatically, but the workflow by hand is:

1. **Reproduce**: `bin/replay_<target> tests/fuzz/artifacts/crash-<sha>` — under
   ASan, which usually names the bug more precisely than the fuzz build does.
2. **Minimise**: `bin/fuzz_<target> -minimize_crash=1 -runs=100000 <input>`.
   A 40 KB crasher and its 12-byte minimisation are the same bug, and only one
   of them is readable in a diff.
3. **File**: move the minimised input to `regress/<target>/`. It is now part of
   `make fuzz-regress` forever, which is what stops the bug coming back.
4. **Fix**, then confirm `make fuzz-regress` passes.
5. Record it in the campaign write-up: what class of bug, which oracle caught it.

A finding is not always a bug in the code under test — sometimes the oracle is
wrong (see `Content-Length` above). Decide which, and write down why, in the
harness next to the check.

## Corpus hygiene

**What is committed is the SEED corpus only** — the 73 files `make fuzz-seed`
writes, one per operation plus the boundary cases the headers call out. What
the fuzzer discovers on top of that is not committed, and that is deliberate:

- Fuzzer inputs are near-random bytes, so git cannot delta-compress them, and
  `fuzz-merge` rewrites *which* thousand files are in the set after every
  campaign. Committing them adds a fresh, undeltable few megabytes per run
  forever - churn, not size, is what makes it a bad trade.
- The seeds are regenerable and stable, so a fresh clone starts from a known
  place rather than an empty one.
- What must never be lost is `regress/`, and it is committed: those inputs are
  findings, they are tiny, and they only ever grow when a real bug is caught.
- Between nightly runs the discovered corpus lives in the CI cache
  (`.github/workflows/fuzz.yml`), so the search still accumulates - it just
  accumulates there instead of in git history.

If you run a long campaign locally, keep its corpus on disk (it makes your next
run start warm) and let `.gitignore` handle it. Only file the crashes.

The ignore rule is `tests/fuzz/corpus/*/*`, which works because gitignore does
not apply to tracked files: the committed seeds stay visible, everything new
stays hidden. Adding a genuinely new seed by hand therefore needs
`git add -f <path>`.

- Re-running `make fuzz-seed` never deletes what the fuzzer found.
- Merge occasionally to keep it minimal:
  `bin/fuzz_<t> -merge=1 corpus/<t>_min corpus/<t> && mv corpus/<t>_min corpus/<t>`
- `make fuzz-cov` says whether the corpus actually reaches the code. An
  uncovered branch after a long campaign is either dead code or a missing
  dictionary token — both worth knowing before claiming a target is "fuzzed".

## The 24-hour run

`make fuzz-long` gives each target 8640 seconds (10 targets ≈ 24 hours). Run it
on a machine nobody needs, from a warm corpus, and keep the output:

```sh
make fuzz-build && make fuzz-long 2>&1 | tee var/log/fuzz-$(date +%F).log
```

Numbers worth reporting afterwards: execs/sec per target, corpus size before and
after, line coverage of each file under test, unique crashes found and fixed, and
whether the run finished clean. "We fuzzed for 24 hours" is a claim about the
clock; coverage and findings are claims about the parser.
