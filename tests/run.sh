#!/bin/sh
# C! test runner
#
#   tests/pass_*.cb     must compile with zero warnings and exit 0
#   tests/fail_*.cb     must be REJECTED by the compiler
#   // expect: TEXT     in a fail test: TEXT must appear in the diagnostic,
#                       so a test cannot pass while the message drifts
#   tests/expected/X.txt  if present, the program's output must match exactly
#
# Every pass test is also checked with -Wall -Wextra on the generated C,
# because leaking C warnings into user builds is a compiler bug.
#
# Usage:  sh tests/run.sh

set -u
cd "$(dirname "$0")/.." || exit 1

CB=${CB:-./build/cbang}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$CB" ]; then
    make >/dev/null || { echo "cannot build cbang"; exit 1; }
fi

pass=0
fail=0

ok()   { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; printf '        %s\n' "$2"; }

# ---------------------------------------------------------------- pass tests
for t in tests/pass_*.cb; do
    name=$(basename "$t" .cb)
    bin="$TMP/$name"

    log=$("$CB" build "$t" -o "$bin" 2>&1)
    if [ $? -ne 0 ]; then
        bad "$name" "did not compile"
        printf '%s\n' "$log" | sed 's/^/        /'
        continue
    fi

    noise=$(printf '%s\n' "$log" | grep -E 'warning|error')
    if [ -n "$noise" ]; then
        bad "$name" "compiler reported:"
        printf '%s\n' "$noise" | sed 's/^/        /'
        continue
    fi

    # generated C must survive the stricter flag set too
    cfile="${t%.cb}.c"
    noise=$(cc -std=c11 -Wall -Wextra -O2 -fsyntax-only "$cfile" 2>&1 | grep -E 'warning|error')
    if [ -n "$noise" ]; then
        bad "$name" "generated C is not clean under -Wall -Wextra:"
        printf '%s\n' "$noise" | sed 's/^/        /'
        continue
    fi

    actual=$("$bin" 2>&1); rc=$?
    if [ "$rc" -ne 0 ]; then
        bad "$name" "exited $rc"
        continue
    fi

    expected_file="tests/expected/$name.txt"
    if [ -f "$expected_file" ]; then
        if [ "$actual" != "$(cat "$expected_file")" ]; then
            bad "$name" "output mismatch"
            printf '        expected: %s\n' "$(cat "$expected_file" | tr '\n' '|')"
            printf '        actual:   %s\n' "$(printf '%s' "$actual" | tr '\n' '|')"
            continue
        fi
    fi

    ok "$name"
    rm -f "$cfile"
done

# --------------------------------------------------------------- fail tests
for t in tests/fail_*.cb; do
    name=$(basename "$t" .cb)
    out=$("$CB" build "$t" -o "$TMP/$name" 2>&1)
    rc=$?
    rm -f "${t%.cb}.c"
    if [ $rc -eq 0 ]; then
        bad "$name" "compiler accepted a program it must reject"
        continue
    fi

    # every "// expect: TEXT" line must show up in the diagnostic,
    # as a whole line of text (not split on spaces)
    missing=""
    sed -n 's|^// expect: ||p' "$t" > "$TMP/exp"
    while IFS= read -r want; do
        [ -z "$want" ] && continue
        printf '%s\n' "$out" | grep -qF -- "$want" || missing="$missing [$want]"
    done < "$TMP/exp"
    if [ -n "$missing" ]; then
        bad "$name" "rejected, but expected text missing:$missing"
        printf '%s\n' "$out" | sed 's/^/        /'
        continue
    fi

    ok "$name (rejected)"
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
