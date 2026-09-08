// Recursion depth and mutual recursion, which stress the frame layout.
#include <stdio.h>

int is_even(int n);
int is_odd(int n);

int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }

long long factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }

int depth(int n) {
  // A local array in every frame, so a wrong frame size corrupts the caller.
  int scratch[8];
  for (int i = 0; i < 8; i++) scratch[i] = n + i;
  if (n == 0) return 0;
  int r = depth(n - 1);
  int sum = 0;
  for (int i = 0; i < 8; i++) sum += scratch[i];
  return r + sum;
}

int hanoi(int n, int from, int to, int via) {
  if (n == 0) return 0;
  return hanoi(n - 1, from, via, to) + 1 + hanoi(n - 1, via, to, from);
}

int main(void) {
  printf("%d %d\n", is_even(10), is_odd(10));
  printf("%lld\n", factorial(12));
  printf("%d\n", depth(50));
  printf("%d\n", hanoi(10, 1, 3, 2));
  return 0;
}
