# cinder -- a C compiler written in C.
#
# There is no configure step and nothing to install. Every source file is one
# translation unit, and the only dependency is a C compiler and libc.

CC      ?= gcc
CFLAGS  ?= -std=c11 -g -O2
WARN     = -Wall -Wno-switch -Wno-unused-function
SRCS     = $(wildcard src/*.c)
BIN      = cinder.exe

.PHONY: all test errors selfhost check bench clean help

all: $(BIN)

$(BIN): $(SRCS) src/cinder.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS)

# The differential suite: every test is compiled by both cinder and gcc and
# run, and the outputs must match.
test: $(BIN)
	@bash test/run.sh

# Programs that must be rejected, and what the message has to say.
errors: $(BIN)
	@bash test/errors.sh

# cinder compiles cinder, and that compiler compiles cinder again. The two
# results must be byte-identical.
selfhost: $(BIN)
	@bash test/selfhost.sh

check: test errors selfhost

bench: $(BIN)
	@echo "cinder compiling its own source:"
	@time (for f in $(SRCS); do ./$(BIN) -Iinclude -Isrc -S $$f -o /dev/null; done)
	@echo "gcc -O0, same files:"
	@time (for f in $(SRCS); do $(CC) -std=c11 -w -O0 -S $$f -o /dev/null; done)

clean:
	rm -f $(BIN) cinder *.o *.s a.exe

help:
	@echo "make            build cinder"
	@echo "make test       differential tests against gcc"
	@echo "make errors     diagnostics tests"
	@echo "make selfhost   compile cinder with cinder, twice, and compare"
	@echo "make check      all three"
	@echo "make bench      compile speed against gcc"
