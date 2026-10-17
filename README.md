# cinder

A C compiler, written in C, that compiles itself.

cinder takes C source and produces x86-64 assembly for Windows, then hands it
to the platform assembler and linker. It implements the lexer, preprocessor,
parser, type system and code generator — everything that constitutes
understanding the language.

```console
$ cinder hello.c -o hello.exe
$ ./hello.exe
hello, world

$ make selfhost
stage 1: building cinder with cinder
stage 2: building cinder with the cinder from stage 1

comparing stage 1 and stage 2
  same  codegen.o
  same  main.o
  same  parse.o
  same  preprocess.o
  same  tokenize.o
  same  type.o
  same  util.o

stage 1 and stage 2 are byte-identical: cinder is self-hosting.
```

## It compiles itself

This is the headline claim, and it is worth being precise about what it means.

Stage 1 is cinder's source compiled by gcc. Stage 2 is the *same source*
compiled by stage 1. If cinder's code generation is correct, the two must be
byte-for-byte identical — they are the same program, compiled from the same
source, by compilers that agree about C. They are.

That test is stronger than the test suite, for a reason worth stating: it
doesn't require anyone to have thought of the case in advance. It exercises
6,800 lines of C using structs, function pointers, varargs, unions, recursion,
`switch`, the preprocessor and every integer width, all at once. A single wrong
instruction anywhere the compiler itself goes breaks the fixed point.

Run it with `make selfhost`.

## Why Windows

Almost every C compiler written as a project targets System V — Linux, macOS,
the ABI every reference describes. cinder targets the **Microsoft x64 calling
convention**, because that is the machine it was written on, and a compiler
whose output you cannot run is a compiler you cannot test.

That turned out to be the most interesting constraint in the project. Five
differences from System V actually bite, and each is a bug you only find by
running the output:

1. **Different argument registers.** RCX, RDX, R8, R9 — not RDI, RSI, RDX, RCX.
2. **RDI and RSI are callee-saved here.** On System V they are argument
   registers, so code that uses them freely as scratch — as a stack-machine
   code generator does — silently corrupts its caller.
3. **The register index is the argument's position, not a per-class counter.**
   In `f(int, double, int)` the arguments go to RCX, XMM1, R8. XMM1, not XMM0,
   because the `double` is the *second* parameter. System V would use XMM0.
4. **Shadow space.** The caller must always allocate 32 bytes above the return
   address, even for a callee taking no arguments at all, because the callee is
   entitled to spill its four register arguments there.
5. **Structs travel in a register only at exactly 1, 2, 4 or 8 bytes.**
   Anything else is passed as a pointer to a caller-made copy, and returned
   through a hidden pointer handed in as argument zero.

Point 4 turns out to be a gift. Because every argument has a reserved home at
`[rsp + i*8]` at the moment of the call, cinder evaluates arguments *straight
into their final slots* rather than pushing and popping them. A nested call
inside an argument list then allocates its own area further down the stack and
cannot disturb the one being built — which is the part that usually goes wrong.

Windows is also **LLP64**: `long` is 4 bytes and `long long` is 8. cinder
follows the platform rather than the more common LP64 assumption, because a
`long` that disagrees with the C runtime you link against makes `printf("%ld")`
print nonsense.

## What it supports

**Types.** `void`, `_Bool`, `char`, `short`, `int`, `long`, `long long`,
`float`, `double`, signed and unsigned, pointers, arrays, structs, unions,
enums, `typedef`, function types and function pointers.

**Expressions.** The full precedence ladder, assignment and compound
assignment, `?:`, comma, pre- and post-increment, `sizeof`, `_Alignof`, casts,
`&&`/`||` with short-circuiting, all the bitwise and shift operators, member
access through `.` and `->`, subscripting, and function calls.

**Statements.** `if`/`else`, `for`, `while`, `do`/`while`, `switch`/`case`/
`default`, `break`, `continue`, `goto` and labels, `return`, blocks.

**Declarations.** Locals, globals, `static` (at both scopes), `extern`,
tentative definitions, nested and partial initialisers, designated initialisers
(`{[2] = x}` and `{.field = y}`), string-literal initialisation of char arrays,
flexible array members, compound literals.

**Variadic functions**, both calling them and defining them, with a
`<stdarg.h>` that is almost entirely ordinary C.

**The preprocessor.** `#include`, object- and function-like `#define` including
variadic macros and the `, ## __VA_ARGS__` extension, `#` stringize, `##`
paste, `#undef`, `#if`/`#elif`/`#else`/`#endif`, `#ifdef`/`#ifndef`,
`defined()`, `#error`, `#warning`, `#pragma once`, and `__FILE__`, `__LINE__`
and `__COUNTER__`.

**GNU extensions** that real code depends on: statement expressions
(`({ ... })`), binary literals, and skipping `__attribute__` and `__declspec`.

## Install and use

Requires a C compiler, plus GNU `as` and `ld` for the output. On Windows,
MinGW-w64 provides all three.

```sh
git clone https://github.com/Nafiz001/cinder
cd cinder
make            # or ./build.sh, if make is not installed
```

```
cinder [options] file...

  -o <file>   write output to <file>
  -S          emit assembly and stop
  -c          compile and assemble, but do not link
  -E          preprocess only
  -I<dir>     add a directory to the include search path
  -D<name>[=<value>]  define a macro
  -U<name>    undefine a macro
  --tokens    dump the token stream
```

`-O`, `-W` and `-g` flags are accepted and ignored, so that a Makefile written
for gcc still runs. cinder does not optimise, and saying so by accepting `-O2`
silently is friendlier than failing.

## How it works

Five stages, one per file.

| File | Lines | Does |
|---|---|---|
| `tokenize.c` | 664 | text → tokens |
| `preprocess.c` | 1,018 | tokens → tokens, macros expanded |
| `parse.c` | 2,668 | tokens → typed AST |
| `type.c` | 308 | type construction and the C conversion rules |
| `codegen.c` | 1,193 | AST → x86-64 assembly |
| `util.c`, `main.c` | 545 | hash map, string helpers, the driver |

### Tokenizing

Less mechanical than it looks, and most of the subtlety in the front end lives
here.

Punctuators must be matched longest-first, or `>>=` lexes as `>` then `>=`. A
`.` may begin a number (`.5`) or be a member access, and `1.5e+10` contains a
`+` that is not an operator — the standard's answer is **preprocessing
numbers**: lex something number-shaped now, decide what it means later.

Keywords are not recognised until *after* preprocessing, because until the
macros are gone an identifier might still turn into one, or stop being one.

Every token records whether it began a line and whether whitespace preceded it.
The parser never looks at either. The preprocessor needs both: `#` only starts
a directive at the beginning of a line, and `f (x)` is a different macro
definition from `f(x)`.

### Preprocessing

It runs on the token list, not on text. That is what the standard describes and
it is the only way to get `##` right — pasting operates on tokens, so `a ## b`
must produce one token that is then re-lexed, and a text-based implementation
cannot tell where the boundaries were.

The subtle part is recursion. `#define f f(x)` must expand once and stop. The
standard's answer is a **hide set**: every token carries the set of macros being
expanded when it was produced, and a macro will not expand a token that already
carries its own name. Function-like macros additionally intersect the hide set
of the closing parenthesis, which is the rule that makes

```c
#define f(x) x
f(f)(1)
```

terminate.

### Parsing

Recursive descent, one function per grammar production, named after it. C's
grammar is small enough to read off the standard, and a hand-written parser can
point at the exact column that broke.

C has exactly one production that is genuinely ambiguous, and it matters: `(x)`
is a cast if `x` names a type and a parenthesised expression otherwise. Deciding
requires a symbol-table lookup, which is why the parser and the symbol table
cannot be separated in C the way they can in most languages.

Two scopes are kept, not one, because C has separate namespaces for ordinary
identifiers and for struct/union/enum tags. That is what lets `struct stat stat;`
compile, and cinder's test suite checks it.

Type specifiers may appear in any order — `unsigned long int` and
`int long unsigned` name the same type. Rather than enumerate the orderings,
each keyword adds a fixed amount to a counter, so every legal spelling sums to
the same number and one `switch` decides them all.

### Types and conversions

The rules people carry wrong assumptions about for years, and getting them right
is most of what separates a compiler from a calculator.

**Integer promotion**: anything narrower than `int` becomes `int` before it is
operated on, which is why `char a = 200, b = 200; a + b` is 400 and not an
overflow. **Signed/unsigned mixing**: when the ranks match and one side is
unsigned, the signed side converts to unsigned, which is why `-1 < 1u` is false.

Both are in the differential test suite, because both are exactly the kind of
thing I would have got wrong from memory.

Conversions are inserted as explicit `ND_CAST` nodes rather than handled inside
the code generator, so they are visible when the tree is dumped and the
generator only ever sees operands that already agree.

### Code generation

A stack machine. Every expression leaves its value in RAX (or XMM0), and a
binary operator evaluates the right side, pushes it, evaluates the left, and
pops. That produces more instructions than a register allocator would — there is
no attempt to keep values in registers across statements — and it is correct by
construction for arbitrarily nested expressions, which is the right trade for a
compiler whose point is to be understood.

Some details that are easy to get wrong and produce plausible-looking output
anyway:

- Arithmetic happens at the operand's own width. Using the 64-bit form for a
  32-bit operation gives the wrong answer on overflow, because C says `int`
  arithmetic wraps at 32 bits.
- `idiv` needs `cqo`/`cdq` first to sign-extend RAX into RDX:RAX. Forgetting it
  leaves whatever was in RDX and yields a wrong quotient or a divide-error trap.
- A signed right shift is `sar` and an unsigned one is `shr`. The wrong one
  turns `-8 >> 1` into a very large positive number.
- Pointer comparison must be unsigned, or an address above 2^63 compares as
  negative.
- NaN compares *unordered*, setting the parity flag. `sete` alone reports
  `NaN == NaN` as true, so the parity flag is folded in explicitly.
- Converting to `_Bool` is a comparison against zero, not a truncation:
  `(_Bool)256` is 1 while `(char)256` is 0.

## Testing

Three suites, run by `make check`.

**Differential tests** (`make test`, 18 programs). Every test is an ordinary C
program that prints things, compiled by both cinder and gcc, run twice, with the
outputs and exit codes required to match.

This is worth much more than hand-written expected output. Writing `expected` by
hand means asserting what I *believe* C does — and the whole category of bug
worth catching is the one where that belief is wrong. gcc has no such problem.
For integer promotion, signed shifts, and the exact type of `'a'`, the oracle is
a compiler that has been right about them for thirty years.

It also caught a test that *I* got wrong: I compared three calls to a counter in
one argument list, and cinder and gcc disagreed. Argument evaluation order is
unspecified in C — gcc goes right to left, cinder goes left to right, and both
are correct. The test was fixed, not the compiler.

**Diagnostics tests** (`make errors`, 26 programs). Each must be rejected, with
a message containing a required fragment and a `file:line`. A compiler is used
more often on code that does not compile than on code that does.

```
example.c:5:   return p.z;
                        ^ no member named 'z'
```

**Self-hosting** (`make selfhost`), described above.

### Bugs the tests actually caught

Worth recording, because they are the ones that would have shipped:

- **`main` returning the wrong value.** The "falling off the end of `main`
  returns 0" instruction sat *after* the return label, so it overwrote the value
  of every explicit `return`. It belongs before the label, where only
  fall-through reaches it.
- **Trailing comma in an initialiser.** `{a, b, c,}` ended the loop on the
  comma and then demanded a brace one token further along. Every keyword table
  in cinder's own source is written that way, so this only surfaced when cinder
  tried to compile itself.
- **Adjacent string literals were never concatenated.** That is translation
  phase 6, and it has to happen after macro expansion, because the strings that
  end up adjacent are usually put there by a macro:
  `#define LOG(fmt, ...) printf("log: " fmt "\n", ...)`.
- **`static` functions were globally visible.** Omitting `.globl` is not
  sufficient on COFF — the `.def` storage class has to say 3
  (`IMAGE_SYM_CLASS_STATIC`) rather than 2 (`..._EXTERNAL`). Two files each
  defining their own `static cast` collided at link time.
- **Struct returns were half-implemented.** Register-sized structs came back in
  RAX as a *value* while the assignment path expected an *address*. Fixed by
  giving every struct-returning call a buffer in the caller's frame, so the call
  always leaves an address behind, like every other struct expression.

## Speed

Compiling cinder's own source, seven files, to assembly, on an Intel i5-1155G7:

| Compiler | Time |
|---|---|
| cinder | 0.26 s |
| `gcc -O0` | 0.93 s |
| `gcc -O2` | 1.99 s |

Roughly 3.6× faster than `gcc -O0`. This is not the achievement it looks like:
cinder does no optimisation, no dataflow analysis, no register allocation and
far less diagnosis than gcc, and it produces considerably worse code. It is here
because a single-pass compiler with no optimiser *should* be fast, and it is
worth confirming that nothing in the design has quietly made it slow.

## Limits

Stated rather than hidden, because they mark where the design ends.

- **It does not optimise.** No register allocation, no constant folding beyond
  what the standard requires for constant expressions, no dead code elimination.
  Every local lives on the stack. Generated code is perhaps 2–4× slower than
  `gcc -O2`.
- **cinder ships its own headers.** MinGW's `stdio.h` is written for gcc and is
  dense with `__declspec`, `__attribute__` and `__inline`. The bundled headers
  declare the same functions with the same prototypes, and because cinder
  follows the platform ABI exactly, those declarations describe the *real* C
  library and the linker resolves against it. What is bundled is the interface;
  the implementation is the platform's own. Only what cinder needs is covered.
- **Windows only.** The parser and preprocessor are portable — the CI builds
  cinder on Linux and macOS — but the code generator emits Microsoft-ABI
  assembly, so its *output* runs on Windows alone.
- **Not implemented**: bitfields, variable-length arrays, `long double` as a
  distinct 80-bit type (it is `double`, as in the MSVC ABI), `_Atomic`,
  thread-local storage, inline assembly, wide and UTF-8 string literals,
  `_Generic`, and K&R-style function definitions.
- **`const` is parsed and ignored.** Enforcing it is a checker, not a code
  generator, and cinder produces correct code without it.
- **No debug information.** `-g` is accepted and ignored, so a debugger will
  show the assembly rather than the source.

## Layout

```
src/cinder.h      shared types: Token, Type, Node, Obj
src/tokenize.c    lexer
src/preprocess.c  the preprocessor
src/parse.c       recursive-descent parser
src/type.c        type system and the conversion rules
src/codegen.c     x86-64 code generation
src/util.c        hash map and string helpers
src/main.c        the driver
include/          the bundled headers
test/run.sh       differential tests against gcc
test/errors.sh    diagnostics tests
test/selfhost.sh  compile cinder with cinder, twice, and compare
examples/         programs to compile, including a Brainfuck interpreter
```

## Examples

`examples/brainfuck.c` is a Brainfuck interpreter — pointers, arrays, a struct,
a switch, a bracket-matching pre-pass and the standard library, in one file. It
is also a test case, so it is compiled by both cinder and gcc on every run and
the outputs are compared.

```console
$ cinder examples/brainfuck.c -o bf.exe
$ ./bf.exe
hello: Hello World!
digits: 0123456789
countdown: 9876543210
```

## References

- ISO/IEC 9899:2011 — the C11 standard
- *x64 calling convention*, Microsoft — the ABI cinder targets
- Kernighan & Ritchie, *The C Programming Language* (1988)
- Aho, Lam, Sethi & Ullman, *Compilers: Principles, Techniques, and Tools*
- Rui Ueyama's *chibicc*, whose staged structure informed how this one is laid
  out

## License

MIT
