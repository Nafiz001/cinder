// Floating point.
#include <stdio.h>

double dadd(double a, double b) { return a + b; }
float fadd(float a, float b) { return a + b; }
double mixed(int a, double b, int c, double d) { return a + b + c + d; }

int main(void) {
  double a = 1.5, b = 2.25;
  printf("%.4f\n", a + b);
  printf("%.4f\n", a - b);
  printf("%.4f\n", a * b);
  printf("%.4f\n", a / b);
  printf("%.4f\n", -a);

  printf("%d %d %d %d\n", a < b, a > b, a == b, a != b);
  printf("%d %d\n", a <= a, a >= b);

  float f = 1.5f;
  printf("%.4f\n", (double)(f * 2));

  printf("%.4f\n", dadd(1.25, 2.5));
  printf("%.4f\n", (double)fadd(1.25f, 2.5f));

  // Four arguments of mixed class: the register index is the position, so the
  // doubles go in XMM1 and XMM3, not XMM0 and XMM1.
  printf("%.4f\n", mixed(1, 2.5, 3, 4.25));

  int i = 3;
  double d = i;
  printf("%.4f\n", d);
  printf("%d\n", (int)(d * 2));

  printf("%.4f\n", 1.0 / 3.0);
  printf("%.6f\n", 0.1 + 0.2);

  double z = 0.0;
  printf("%d\n", z == 0.0);
  printf("%d\n", !z);

  if (a) printf("nonzero is true\n");
  if (!z) printf("zero is false\n");

  // A float argument to a variadic function is promoted to double.
  printf("%.4f %.4f\n", 1.5f, 2.5);
  return 0;
}
