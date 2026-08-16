# Prompt: Generate Unity Unit Tests

Generate Unity unit tests for the pure helper described below. Output one C file that compiles and runs against the Unity framework.

## Rules

1. Output a single C file only. No prose, no markdown fences, no explanation around the code.
2. The file must follow this structure:
   - `#include "unity.h"`
   - `#include "<helper>.h"`
   - Any other standard library includes needed
   - `void setUp(void) {}` and `void tearDown(void) {}` (Unity requires these)
   - Each test as a separate `void test_xxx(void)` function
   - A `main()` that calls `UNITY_BEGIN()`, then `RUN_TEST(...)` for each test, then `return UNITY_END();`
3. Cover normal cases, edge cases, and invalid input.
4. Do NOT write tests that call `fork()`, `execv()`, `execvp()`, `waitpid()`, the interactive shell loop, or anything that reads from stdin. Those are covered by integration tests written by the student.
5. Each test function name must start with `test_`.
6. Add a comment at the top of the file marking it as an AI-generated draft, with a note that the student is responsible for reviewing and modifying it.
7. Keep tests small and readable. One assertion concept per test where possible.

## What you may use

- Unity assertions: `TEST_ASSERT_EQUAL_INT`, `TEST_ASSERT_EQUAL_STRING`, `TEST_ASSERT_NULL`, `TEST_ASSERT_NOT_NULL`, `TEST_ASSERT_TRUE`, `TEST_ASSERT_FALSE`, and similar. See `tests/unity/unity.h` if unsure.
- Standard C library functions for setting up test inputs.

## What you must not do

- Do not test private (static) functions. Test only what is declared in the header.
- Do not invent behaviour. If the header does not specify behaviour for some input, ask the student rather than guess.
- Do not include or link against `source/shell.c`. It has its own `main()` and will conflict.
- Do not modify any file outside `tests/unit/`.

## Edge cases worth considering

For any string-parsing helper:

- Empty input
- Whitespace-only input
- Input with only delimiters
- Input with leading and trailing whitespace
- Input with a trailing newline or carriage return
- Input that matches a prefix of the expected pattern but is not the pattern itself (for example `PATHETIC` vs `PATH=`)
- Input at exactly the boundary of any size limit declared in the header

For any lookup or classifier:

- Each valid input value
- An invalid input value
- An empty string

For any splitter or extractor:

- The well-formed input
- Missing delimiter
- Empty key, empty value, or both
- Multiple delimiters
