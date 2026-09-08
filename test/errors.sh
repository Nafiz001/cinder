#!/usr/bin/env bash
#
# Diagnostics tests.
#
# A compiler is used far more often on code that does not compile than on code
# that does, so the error messages are part of the product rather than a
# consolation prize. Each case here is a program that must be rejected, paired
# with a fragment the message has to contain.
#
# The check is deliberately loose -- a substring, not the whole message -- so
# that rewording a diagnostic does not break the suite, while silently losing
# the ability to detect the mistake does.

set -u

cd "$(dirname "$0")/.."

CINDER=${CINDER:-./cinder.exe}
TMP="${TEMP:-/tmp}/cinder-errors-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# check <name> <expected substring> <source>
check() {
  local name="$1" want="$2" src="$3"
  printf '%s' "$src" >"$TMP/$name.c"

  if "$CINDER" -Iinclude -S "$TMP/$name.c" -o "$TMP/$name.s" >"$TMP/$name.err" 2>&1; then
    echo "FAIL $name (compiled, but should have been rejected)"
    fail=$((fail + 1))
    return
  fi

  if ! grep -qF -- "$want" "$TMP/$name.err"; then
    echo "FAIL $name (message did not mention: $want)"
    sed 's/^/      /' "$TMP/$name.err" | head -4
    fail=$((fail + 1))
    return
  fi

  # A diagnostic that does not say where it happened is barely a diagnostic.
  if ! grep -q "^.*:[0-9]*:" "$TMP/$name.err"; then
    echo "FAIL $name (message carried no file:line)"
    sed 's/^/      /' "$TMP/$name.err" | head -4
    fail=$((fail + 1))
    return
  fi

  pass=$((pass + 1))
}

check undefined_var "undefined variable" \
  'int main(void) { return nosuchthing; }'

check undefined_func "implicit declaration" \
  'int main(void) { return nosuchcall(1); }'

check missing_semicolon "expected ';'" \
  'int main(void) { int x = 1 return x; }'

check missing_paren "expected ')'" \
  'int main(void) { return (1 + 2; }'

check missing_brace "expected" \
  'int main(void) { if (1) { return 1; '

check bad_member "no member named" \
  'struct S { int a; }; int main(void) { struct S s; return s.b; }'

check not_a_struct "not a struct or union" \
  'int main(void) { int x = 1; return x.field; }'

check deref_non_pointer "invalid pointer dereference" \
  'int main(void) { int x = 1; return *x; }'

check void_variable "cannot have type void" \
  'int main(void) { void v; return 0; }'

check add_two_pointers "cannot add two pointers" \
  'int main(void) { int a, b; int *p = &a, *q = &b; return (int)(p + q); }'

check bad_type_combo "invalid combination of type specifiers" \
  'int main(void) { float int x; return 0; }'

check unknown_enum "unknown enum type" \
  'int main(void) { enum NoSuch e; return 0; }'

check break_outside_loop "break outside" \
  'int main(void) { break; return 0; }'

check continue_outside_loop "continue outside" \
  'int main(void) { continue; return 0; }'

check case_outside_switch "case outside" \
  'int main(void) { case 1: return 0; }'

check undeclared_label "undeclared label" \
  'int main(void) { goto nowhere; return 0; }'

check redefinition "redefinition" \
  'int f(void) { return 1; } int f(void) { return 2; } int main(void){return f();}'

check unterminated_string "unterminated string literal" \
  'int main(void) { char *s = "no closing quote
 ; return 0; }'

check unterminated_comment "unterminated block comment" \
  'int main(void) { /* never closed
 return 0; }'

check bad_directive "invalid preprocessor directive" \
  '#nonsense
int main(void) { return 0; }'

check unterminated_if "unterminated conditional" \
  '#if 1
int main(void) { return 0; }'

check stray_endif "stray #endif" \
  '#endif
int main(void) { return 0; }'

check error_directive "#error" \
  '#error this is deliberate
int main(void) { return 0; }'

check missing_include "cannot open" \
  '#include <no_such_header_anywhere.h>
int main(void) { return 0; }'

check not_constant "not a compile-time constant" \
  'int f(void); int a[f()]; int main(void) { return 0; }'

check not_a_function "not a function" \
  'int main(void) { int x = 1; return x(); }'

echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ] || exit 1
