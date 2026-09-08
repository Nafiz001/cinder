#!/usr/bin/env bash
#
# The test suite is differential: every test is an ordinary C program that
# prints things, compiled twice -- once by cinder and once by gcc -- and run
# both times. The outputs and the exit codes must match.
#
# This is worth more than a suite of hand-written expected outputs. Writing
# `expected` by hand means asserting what I *believe* C does, and the whole
# category of bug worth catching here is the one where that belief is wrong.
# gcc has no such problem. Integer promotion, signed shifts, the exact value of
# `(char)200 + (char)200` -- for all of these the oracle is a compiler that has
# been right about them for thirty years.
#
# usage: test/run.sh [pattern]

set -u

cd "$(dirname "$0")/.."

CINDER=./cinder.exe
TMP="${TEMP:-/tmp}/cinder-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pattern="${1:-}"

pass=0
fail=0
failed_names=()

for src in test/cases/*.c; do
  name=$(basename "$src" .c)

  if [ -n "$pattern" ] && [[ "$name" != *"$pattern"* ]]; then
    continue
  fi

  # gcc first: if the oracle cannot build it, the test itself is wrong and
  # saying so is more useful than reporting a cinder failure.
  if ! gcc -std=c11 -w -o "$TMP/$name.gcc.exe" "$src" 2>"$TMP/$name.gcc.err"; then
    echo "SKIP $name (gcc could not build the test)"
    sed 's/^/      /' "$TMP/$name.gcc.err" | head -5
    continue
  fi

  "$TMP/$name.gcc.exe" >"$TMP/$name.gcc.out" 2>&1
  gcc_status=$?

  if ! "$CINDER" -Iinclude -o "$TMP/$name.cinder.exe" "$src" \
       >"$TMP/$name.cinder.err" 2>&1; then
    echo "FAIL $name (cinder could not compile it)"
    sed 's/^/      /' "$TMP/$name.cinder.err" | head -12
    fail=$((fail + 1))
    failed_names+=("$name")
    continue
  fi

  "$TMP/$name.cinder.exe" >"$TMP/$name.cinder.out" 2>&1
  cinder_status=$?

  if [ "$gcc_status" != "$cinder_status" ]; then
    echo "FAIL $name (exit code: gcc $gcc_status, cinder $cinder_status)"
    fail=$((fail + 1))
    failed_names+=("$name")
    continue
  fi

  if ! diff -q "$TMP/$name.gcc.out" "$TMP/$name.cinder.out" >/dev/null; then
    echo "FAIL $name (output differs)"
    diff "$TMP/$name.gcc.out" "$TMP/$name.cinder.out" | head -20 |
      sed 's/^/      /'
    fail=$((fail + 1))
    failed_names+=("$name")
    continue
  fi

  pass=$((pass + 1))
done

echo
echo "passed $pass, failed $fail"

if [ "$fail" -gt 0 ]; then
  echo "failed: ${failed_names[*]}"
  exit 1
fi
