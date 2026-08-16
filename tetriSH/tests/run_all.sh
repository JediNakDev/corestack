#!/bin/sh

mode=${1:-local}
shift

passed_cases=0
failed_cases=0
skipped_cases=0
total_cases=0
passed_suites=0
failed_suites=0
total_suites=0

for suite in "$@"; do
    total_suites=$((total_suites + 1))
    output=$(mktemp "${TMPDIR:-/tmp}/tetrish-test.XXXXXX") || exit 2
    if [ "$mode" = "ci" ] && { [ "$suite" = "bin/test_db" ] || [ "$suite" = "bin/test_auth" ] || [ "$suite" = "bin/test_history" ] || [ "$suite" = "bin/test_race_cond" ]; }; then
        TETRISH_NO_RUNNER=1 "./$suite" >"$output" 2>&1
    else
        "./$suite" >"$output" 2>&1
    fi
    status=$?
    cat "$output"

    summary=$(awk '/^Summary: [0-9][0-9]* passed, [0-9][0-9]* failed, [0-9][0-9]* skipped, [0-9][0-9]* total$/ { line = $0 } END { print line }' "$output")
    rm -f "$output"

    summary_valid=0
    suite_failed_cases=0
    if [ -n "$summary" ]; then
        set -- $summary
        suite_passed_cases=$2
        suite_failed_cases=$4
        suite_skipped_cases=$6
        suite_total_cases=$8
        if [ $((suite_passed_cases + suite_failed_cases + suite_skipped_cases)) -eq "$suite_total_cases" ]; then
            summary_valid=1
            passed_cases=$((passed_cases + suite_passed_cases))
            failed_cases=$((failed_cases + suite_failed_cases))
            skipped_cases=$((skipped_cases + suite_skipped_cases))
            total_cases=$((total_cases + suite_total_cases))
        fi
    fi
    if [ "$summary_valid" -eq 0 ]; then
        failed_cases=$((failed_cases + 1))
        total_cases=$((total_cases + 1))
    fi

    if [ "$status" -eq 0 ] && [ "$summary_valid" -eq 1 ] && [ "$suite_failed_cases" -eq 0 ]; then
        passed_suites=$((passed_suites + 1))
        printf 'PASS  %s\n' "$suite"
    else
        failed_suites=$((failed_suites + 1))
        printf 'FAIL  %s\n' "$suite"
    fi
done

printf '\nTest suite summary\n'
printf 'Summary: %d passed, %d failed, %d skipped, %d total\n' \
    "$passed_cases" "$failed_cases" "$skipped_cases" "$total_cases"
printf 'Suites: %d passed, %d failed, %d total\n' \
    "$passed_suites" "$failed_suites" "$total_suites"

[ "$failed_suites" -eq 0 ]
