// Function pointers, arrays of them, and typedefs of them.
#include <stdio.h>

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }

typedef int (*BinOp)(int, int);

int apply(BinOp op, int a, int b) { return op(a, b); }

struct Handler { const char *name; BinOp fn; };

int main(void) {
  int (*fp)(int, int) = add;
  printf("%d\n", fp(3, 4));
  printf("%d\n", (*fp)(3, 4));

  BinOp ops[3] = {add, sub, mul};
  for (int i = 0; i < 3; i++) printf("%d ", ops[i](6, 3));
  printf("\n");

  printf("%d\n", apply(sub, 10, 4));

  struct Handler handlers[3] = {{"add", add}, {"sub", sub}, {"mul", mul}};
  for (int i = 0; i < 3; i++)
    printf("%s=%d ", handlers[i].name, handlers[i].fn(8, 2));
  printf("\n");

  fp = ops[2];
  printf("%d\n", fp(5, 5));
  return 0;
}
