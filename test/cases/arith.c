// Arithmetic, precedence, and the integer conversions.
#include <stdio.h>

int main(void) {
  printf("%d\n", 1 + 2 * 3);
  printf("%d\n", (1 + 2) * 3);
  printf("%d\n", 100 / 7);
  printf("%d\n", 100 % 7);
  printf("%d\n", -100 / 7);
  printf("%d\n", -100 % 7);
  printf("%d\n", 1 << 10);
  printf("%d\n", -8 >> 1);
  printf("%u\n", (unsigned)-8 >> 1);
  printf("%d\n", 0xff & 0x0f);
  printf("%d\n", 0xf0 | 0x0f);
  printf("%d\n", 0xff ^ 0x0f);
  printf("%d\n", ~0);
  printf("%d\n", !0);
  printf("%d\n", !5);

  // Integer promotion: two chars are promoted to int before adding, so this
  // is 400 and not an overflow.
  char a = 200, b = 200;
  printf("%d\n", a + b);

  // The classic signed/unsigned surprise.
  printf("%d\n", -1 < 1u);

  // Signed overflow wraps at 32 bits, not 64.
  int big = 2147483647;
  printf("%d\n", big + 1);

  printf("%d\n", 7 && 0);
  printf("%d\n", 7 || 0);
  printf("%d\n", 3 == 3);
  printf("%d\n", 3 != 3);
  printf("%d\n", 3 < 4);
  printf("%d\n", 3 <= 3);
  printf("%d\n", 3 > 4);
  printf("%d\n", 3 >= 3);
  printf("%d\n", 1 ? 2 : 3);
  printf("%d\n", 0 ? 2 : 3);

  int x = 5;
  printf("%d %d\n", x++, x);
  printf("%d %d\n", ++x, x);
  printf("%d %d\n", x--, x);
  printf("%d %d\n", --x, x);

  x = 10;
  x += 5;  printf("%d\n", x);
  x -= 3;  printf("%d\n", x);
  x *= 2;  printf("%d\n", x);
  x /= 4;  printf("%d\n", x);
  x %= 4;  printf("%d\n", x);
  x <<= 3; printf("%d\n", x);
  x >>= 1; printf("%d\n", x);
  x &= 6;  printf("%d\n", x);
  x |= 9;  printf("%d\n", x);
  x ^= 3;  printf("%d\n", x);

  int i = 0, j = (i = 3, i + 1);
  printf("%d %d\n", i, j);
  return 0;
}
