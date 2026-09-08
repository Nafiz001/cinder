// Edge cases that a stack-machine code generator gets wrong if the depth
// bookkeeping or the operand width is off.
#include <stdio.h>

int f0(void) { return 1; }
int f1(int a) { return a; }
int f4(int a, int b, int c, int d) { return a * 1000 + b * 100 + c * 10 + d; }

int main(void) {
  // Deep nesting: every level pushes and must pop exactly once.
  printf("%d\n", ((((1 + 2) * (3 + 4)) - ((5 + 6) * 2)) + 100));
  printf("%d\n", 1 + (2 + (3 + (4 + (5 + (6 + (7 + (8 + 9))))))));
  printf("%d\n", f4(f1(1), f1(2), f1(3), f1(4)));
  printf("%d\n", f4(f4(1,2,3,4), 0, 0, 0) + f0());

  // Nested ternaries.
  int x = 5;
  printf("%d\n", x < 3 ? 1 : x < 7 ? 2 : 3);
  printf("%d\n", (x > 0 ? (x > 10 ? 100 : 10) : -1));

  // Short-circuit: the right side must not run.
  int side = 0;
  if (0 && (side = 1)) ;
  printf("%d\n", side);
  if (1 || (side = 2)) ;
  printf("%d\n", side);

  // Compound assignment through a pointer must evaluate the address once.
  int arr[3] = {10, 20, 30};
  int *p = arr;
  *p++ += 5;
  printf("%d %d %d\n", arr[0], arr[1], (int)(p - arr));

  int i = 0;
  arr[i++] = 99;
  printf("%d %d\n", arr[0], i);

  // Unsigned arithmetic and comparison.
  unsigned a = 3000000000u;
  unsigned b = 1000000000u;
  printf("%u %u %u\n", a + b, a - b, a / b);
  printf("%d %d\n", a > b, (int)a > (int)b);

  // 64-bit values.
  long long big = 1234567890123LL;
  printf("%lld %lld %lld\n", big, big * 2, big / 1000);
  unsigned long long ubig = 18446744073709551615ULL;
  printf("%llu\n", ubig);
  printf("%llu\n", ubig / 3);

  // Narrow types keep their width through arithmetic.
  char c = 100;
  short s = 30000;
  printf("%d %d\n", c * 2, s * 2);
  c = c * 2;
  s = s * 2;
  printf("%d %d\n", c, s);

  // Modulo with negative operands.
  printf("%d %d %d %d\n", 7 / 2, -7 / 2, 7 % 2, -7 % 2);

  // A switch on a negative and a sparse value.
  for (int k = -2; k < 3; k++) {
    switch (k) {
    case -2: printf("neg2\n"); break;
    case 1000: printf("never\n"); break;
    case 2: printf("two\n"); break;
    default: printf("other %d\n", k);
    }
  }

  // A goto that jumps forward over code.
  goto skip;
  printf("not printed\n");
skip:
  printf("jumped\n");

  // An empty for loop body and an empty statement.
  int n = 0;
  for (int j = 0; j < 5; j++)
    ;
  while (n < 3) n++;
  printf("%d\n", n);
  return 0;
}
