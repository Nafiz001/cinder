// Functions: recursion, many arguments, pointers to functions.
#include <stdio.h>

int add(int a, int b) { return a + b; }

int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// More than four parameters, so some arrive on the stack.
int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {
  return a + b + c + d + e + f + g + h;
}

int sum10(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
  return a + b + c + d + e + f + g + h + i + j;
}

static int hidden(int x) { return x * 3; }

void nothing(void) { printf("void function\n"); }

int apply(int (*fn)(int, int), int a, int b) { return fn(a, b); }

int mul(int a, int b) { return a * b; }

int ackermann(int m, int n) {
  if (m == 0) return n + 1;
  if (n == 0) return ackermann(m - 1, 1);
  return ackermann(m - 1, ackermann(m, n - 1));
}

int main(void) {
  printf("%d\n", add(3, 4));
  printf("%d\n", fib(20));
  printf("%d\n", sum8(1, 2, 3, 4, 5, 6, 7, 8));
  printf("%d\n", sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  printf("%d\n", hidden(5));
  nothing();
  printf("%d\n", apply(add, 3, 4));
  printf("%d\n", apply(mul, 3, 4));
  printf("%d\n", ackermann(2, 3));

  // A nested call inside an argument list, which is where a broken outgoing
  // argument area shows up.
  printf("%d\n", add(add(1, 2), add(3, 4)));
  printf("%d\n", sum8(add(1, 1), 2, 3, 4, 5, 6, 7, add(4, 4)));
  return 0;
}
