#!/usr/bin/env bash
# Build cinder. There is no configure step and nothing to install: it is one
# translation unit per source file and no dependencies beyond libc.
set -eu
cd "$(dirname "$0")"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--std=c11 -g -O2 -Wall -Wno-switch -Wno-unused-function}
$CC $CFLAGS -o cinder.exe src/*.c
echo "built cinder.exe"
