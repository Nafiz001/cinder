// Variadic function definitions.
#include <stdio.h>
#include <stdarg.h>

int sum(int count, ...) {
  va_list ap;
  va_start(ap, count);
  int total = 0;
  for (int i = 0; i < count; i++)
    total += va_arg(ap, int);
  va_end(ap);
  return total;
}

double dsum(int count, ...) {
  va_list ap;
  va_start(ap, count);
  double total = 0;
  for (int i = 0; i < count; i++)
    total += va_arg(ap, double);
  va_end(ap);
  return total;
}

// Mixed types read in order.
void describe(const char *label, ...) {
  va_list ap;
  va_start(ap, label);
  int i = va_arg(ap, int);
  double d = va_arg(ap, double);
  char *s = va_arg(ap, char *);
  va_end(ap);
  printf("%s: %d %.2f %s\n", label, i, d, s);
}

// Forwarding a va_list to a library function, which is what any logging
// wrapper does.
int logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

int main(void) {
  printf("%d\n", sum(3, 1, 2, 3));
  printf("%d\n", sum(5, 10, 20, 30, 40, 50));
  printf("%d\n", sum(0));
  printf("%.2f\n", dsum(3, 1.5, 2.5, 3.0));
  describe("mixed", 42, 3.14, "text");
  logf_("forwarded %d and %s\n", 7, "str");
  return 0;
}
