#!/usr/bin/env bash
#
# The self-hosting test: cinder compiles cinder, and then that compiler
# compiles cinder again.
#
# This is the strongest correctness check available to a compiler, and it is
# strong for a specific reason. Stage 1 is cinder's source compiled by gcc;
# stage 2 is the same source compiled by stage 1. If cinder's code generation
# is correct, stage 2 must be byte-for-byte identical to stage 1 -- both are
# the same program, compiled from the same source, by compilers that agree.
#
# A single wrong instruction anywhere on a path the compiler itself takes
# breaks this. And unlike a test suite, it does not need anyone to have thought
# of the case in advance: the compiler is thirteen thousand lines of C
# exercising structs, pointers, function pointers, varargs, recursion, unions,
# switch, the preprocessor and every integer width, all at once.
#
# It is also why a difference here is worth more than a failure: the two stages
# differing tells you cinder miscompiles something, without telling you what,
# which is the honest starting point for a real debugging session.

set -eu

cd "$(dirname "$0")/.."

CC=${CC:-gcc}
WORK="${TEMP:-/tmp}/cinder-selfhost-$$"
mkdir -p "$WORK/stage1" "$WORK/stage2"
trap 'rm -rf "$WORK"' EXIT

echo "stage 0: building cinder with $CC"
$CC -std=c11 -g -O2 -w -o "$WORK/cinder-stage0.exe" src/*.c

echo "stage 1: building cinder with cinder"
for f in src/*.c; do
  name=$(basename "$f" .c)
  "$WORK/cinder-stage0.exe" -Iinclude -Isrc -c "$f" -o "$WORK/stage1/$name.o"
done
$CC -o "$WORK/cinder-stage1.exe" "$WORK"/stage1/*.o

echo "stage 2: building cinder with the cinder from stage 1"
for f in src/*.c; do
  name=$(basename "$f" .c)
  "$WORK/cinder-stage1.exe" -Iinclude -Isrc -c "$f" -o "$WORK/stage2/$name.o"
done
$CC -o "$WORK/cinder-stage2.exe" "$WORK"/stage2/*.o

echo
echo "comparing stage 1 and stage 2"

differing=0
for f in src/*.c; do
  name=$(basename "$f" .c)
  if cmp -s "$WORK/stage1/$name.o" "$WORK/stage2/$name.o"; then
    echo "  same  $name.o"
  else
    echo "  DIFFER $name.o"
    differing=$((differing + 1))
  fi
done

echo
if [ "$differing" -ne 0 ]; then
  echo "FAILED: $differing object file(s) differ; cinder does not reach a fixed point"
  exit 1
fi

echo "stage 1 and stage 2 are byte-identical: cinder is self-hosting."
echo
echo "running the test suite with the self-compiled compiler"
CINDER="$WORK/cinder-stage1.exe" ./test/run.sh
