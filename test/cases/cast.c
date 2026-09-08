// Casts and conversions.
#include <stdio.h>

int main(void) {
  printf("%d\n", (int)3.9);
  printf("%d\n", (int)-3.9);
  printf("%d\n", (char)300);
  printf("%d\n", (unsigned char)300);
  printf("%d\n", (short)70000);
  printf("%d\n", (int)(char)-1);
  printf("%u\n", (unsigned)-1);
  printf("%lld\n", (long long)(int)-1);
  printf("%llu\n", (unsigned long long)(unsigned)-1);

  printf("%d\n", (int)(_Bool)0);
  printf("%d\n", (int)(_Bool)5);
  // Converting to _Bool is a comparison, not a truncation: 256 is true even
  // though (char)256 is 0.
  printf("%d\n", (int)(_Bool)256);

  double d = 3.75;
  printf("%d\n", (int)d);
  float f = (float)d;
  printf("%d\n", (int)f);

  int i = 7;
  printf("%.4f\n", (double)i / 2);
  printf("%d\n", i / 2);

  long long big = 1;
  big = big << 40;
  printf("%lld\n", big);
  printf("%d\n", (int)big);

  unsigned u = 4294967295u;
  printf("%u %d\n", u, (int)u);

  char narrow = 200;
  printf("%d\n", narrow);
  unsigned char wide = 200;
  printf("%d\n", wide);
  return 0;
}
