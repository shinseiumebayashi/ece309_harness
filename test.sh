#!/usr/bin/env bash
# ============================================================
# test.sh -- automated tests for the ECE 309 mini-harness
#
# Usage:  bash test.sh
# Exit:   0 if every case passed, 1 otherwise.
#
# No human interaction: every case pipes a fixed script of input
# lines into the program and inspects the output with grep.
# ============================================================

# Note: we deliberately do NOT use `set -e`. A failing test must be
# recorded and reported, not abort the whole run.

BIN=./harness           # the program under test
SRC=harness.c           # source, needed for the sanitizer rebuild
DBG=./harness_debug     # sanitizer build
FAILURES=0              # running count of failed cases

# ------------------------------------------------------------
# pass / fail helpers
# ------------------------------------------------------------
pass() {
    printf 'PASS  %s\n' "$1"
}

fail() {
    printf 'FAIL  %s\n' "$1"
    FAILURES=$((FAILURES + 1))
}

# ------------------------------------------------------------
# check_contains <name> <input> <pattern>
#   Runs the harness with <input> on stdin and passes if the output
#   contains <pattern>.
# ------------------------------------------------------------
check_contains() {
    local name="$1" input="$2" pattern="$3" out

    out=$(printf '%s' "$input" | "$BIN" 2>&1)

    if printf '%s' "$out" | grep -q -- "$pattern"; then
        pass "$name"
    else
        fail "$name  (expected output to contain: $pattern)"
        printf '      --- actual output ---\n'
        printf '%s\n' "$out" | sed 's/^/      /'
    fi
}

# ------------------------------------------------------------
# check_contains_and_status <name> <input> <pattern>
#   As above, but also requires an exit status of 0. Used for the
#   error cases: the harness must report the problem and still shut
#   down cleanly rather than crashing.
# ------------------------------------------------------------
check_contains_and_status() {
    local name="$1" input="$2" pattern="$3" out status

    out=$(printf '%s' "$input" | "$BIN" 2>&1)
    status=$?

    if ! printf '%s' "$out" | grep -q -- "$pattern"; then
        fail "$name  (expected output to contain: $pattern)"
        printf '      --- actual output ---\n'
        printf '%s\n' "$out" | sed 's/^/      /'
        return
    fi

    if [ "$status" -ne 0 ]; then
        fail "$name  (expected exit status 0, got $status)"
        return
    fi

    pass "$name"
}

# ------------------------------------------------------------
# Preflight: the binary has to exist before anything else runs.
# ------------------------------------------------------------
if [ ! -x "$BIN" ]; then
    printf 'Building %s ...\n' "$BIN"
    if ! gcc -Wall -Wextra "$SRC" -o harness; then
        printf 'FATAL: could not compile %s\n' "$SRC"
        exit 1
    fi
fi

printf '=== functional tests ===\n'

# ---- 1. greeting ----
check_contains \
    "1. greeting" \
    'hello
exit
' \
    '\[model\] Hello!'

# ---- 2. tool: arithmetic ----
check_contains \
    "2. tool arithmetic" \
    'calc: 3 + 4
exit
' \
    '\[tool\] result = 7'

# ---- 3. tool: division by zero, must not crash ----
check_contains_and_status \
    "3. division by zero survives" \
    'calc: 10 / 0
exit
' \
    'division by zero'

# ---- 4. tool: unparseable expression, must not crash ----
check_contains_and_status \
    "4. parse error survives" \
    'calc: banana
exit
' \
    'could not parse'

# ---- 5. ring buffer drops the oldest turns ----
# Eight turns go in; the history holds five, so a, b and c must be gone
# while d through h remain.
RING_INPUT='a
b
c
d
e
f
g
h
history
exit
'

RING_OUT=$(printf '%s' "$RING_INPUT" | "$BIN" 2>&1)

if printf '%s' "$RING_OUT" | grep -q 'user : d' \
   && ! printf '%s' "$RING_OUT" | grep -q 'user : a'; then
    pass "5. ring buffer keeps 5 turns, drops the oldest"
else
    fail "5. ring buffer (expected 'user : d' present and 'user : a' absent)"
    printf '      --- actual output ---\n'
    printf '%s\n' "$RING_OUT" | sed 's/^/      /'
fi

# ---- 6. EOF without an explicit exit ----
printf 'hello
' | "$BIN" > /dev/null 2>&1
EOF_STATUS=$?

if [ "$EOF_STATUS" -eq 0 ]; then
    pass "6. clean shutdown on EOF"
else
    fail "6. clean shutdown on EOF (exit status $EOF_STATUS)"
fi

# ------------------------------------------------------------
# Memory check
#
# valgrind is not available on Apple Silicon, so we use the
# AddressSanitizer that ships with clang instead. LeakSanitizer is
# not enabled by default on macOS, so we request it explicitly via
# ASAN_OPTIONS; on Linux it is on already and the flag is harmless.
# ------------------------------------------------------------
# ---- 7. model-driven tool call (v2) ----
# The model must ask for the tool by emitting the sentinel, the harness
# must run it, and the model must then phrase a final answer. All three
# stages have to appear for this to pass.
V2_OUT=$(printf 'what is 12 * 5\nexit\n' | "$BIN" 2>&1)

if printf '%s' "$V2_OUT" | grep -q 'TOOL_CALL: multiply(12, 5)' \
   && printf '%s' "$V2_OUT" | grep -q 'result = 60' \
   && printf '%s' "$V2_OUT" | grep -q 'The answer is 60'; then
    pass "7. model-driven tool call round trip"
else
    fail "7. model-driven tool call round trip"
    printf '      --- actual output ---\n'
    printf '%s\n' "$V2_OUT" | sed 's/^/      /'
fi

# ---- 8. word-boundary fix for the greeting ----
# v1 greeted "this is a test" because strstr found the "hi" in "this".
check_contains \
    "8. 'hi' no longer matches inside 'this'" \
    'this is a test
exit
' \
    'I received: this is a test'

# ---- 9. the tool refusing does not crash the round trip ----
check_contains_and_status \
    "9. model-driven division by zero survives" \
    'what is 10 / 0
exit
' \
    'could not be completed'

printf '\n=== memory check ===\n'

if [ "$(uname)" = "Darwin" ]; then
    # LeakSanitizer is not implemented on macOS -- ASAN_OPTIONS=detect_leaks=1
    # is silently ignored there, so the check would inspect nothing at all.
    # Apple's own leaks(1) is used instead. This was found by deliberately
    # deleting a free() and watching the test still pass.
    printf 'platform: macOS -- using leaks(1)\n'

    if gcc -g "$SRC" -o harness_debug 2>/dev/null; then
        LEAK_OUT=$(printf '%s' "$RING_INPUT" \
            | MallocStackLogging=1 leaks --atExit -- "$DBG" 2>&1)

        if printf '%s' "$LEAK_OUT" | grep -q '0 leaks for 0 total leaked bytes'; then
            pass "10. no leaks"
        else
            fail "10. no leaks"
            printf '      --- leaks output ---\n'
            printf '%s\n' "$LEAK_OUT" | grep -i 'leak' | sed 's/^/      /'
        fi
    else
        fail "10. could not build the debug binary"
    fi
else
    # Linux: AddressSanitizer includes LeakSanitizer.
    printf 'platform: %s -- using AddressSanitizer\n' "$(uname)"

    if gcc -fsanitize=address -g "$SRC" -o harness_debug 2>/dev/null; then
        ASAN_OUT=$(printf '%s' "$RING_INPUT" \
            | ASAN_OPTIONS=detect_leaks=1 "$DBG" 2>&1)

        if printf '%s' "$ASAN_OUT" | grep -q 'LeakSanitizer\|ERROR: AddressSanitizer'; then
            fail "10. no leaks or memory errors"
            printf '      --- sanitizer output ---\n'
            printf '%s\n' "$ASAN_OUT" | grep -A 20 'LeakSanitizer\|ERROR: AddressSanitizer' \
                | sed 's/^/      /'
        else
            pass "10. no leaks or memory errors"
        fi
    else
        fail "10. could not build the sanitizer binary"
    fi
fi

# ------------------------------------------------------------
# Summary
# ------------------------------------------------------------
printf '\n=== summary ===\n'

if [ "$FAILURES" -eq 0 ]; then
    printf 'All tests passed.\n'
    exit 0
else
    printf '%d test(s) failed.\n' "$FAILURES"
    exit 1
fi
