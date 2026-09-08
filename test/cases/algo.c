// A few real algorithms, which exercise the whole compiler at once rather
// than one feature at a time.
#include <stdio.h>
#include <string.h>

void bubble_sort(int *a, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - 1 - i; j++)
      if (a[j] > a[j + 1]) {
        int t = a[j];
        a[j] = a[j + 1];
        a[j + 1] = t;
      }
}

int binary_search(int *a, int n, int target) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (a[mid] == target) return mid;
    if (a[mid] < target) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

void reverse(char *s) {
  int n = (int)strlen(s);
  for (int i = 0; i < n / 2; i++) {
    char t = s[i];
    s[i] = s[n - 1 - i];
    s[n - 1 - i] = t;
  }
}

int is_prime(int n) {
  if (n < 2) return 0;
  for (int i = 2; i * i <= n; i++)
    if (n % i == 0) return 0;
  return 1;
}

struct Node { int val; struct Node *next; };

int main(void) {
  int a[10] = {5, 2, 9, 1, 7, 3, 8, 6, 4, 0};
  bubble_sort(a, 10);
  for (int i = 0; i < 10; i++) printf("%d ", a[i]);
  printf("\n");

  printf("%d %d\n", binary_search(a, 10, 7), binary_search(a, 10, 99));
  printf("%d %d\n", gcd(48, 18), gcd(17, 5));

  char s[] = "compiler";
  reverse(s);
  printf("%s\n", s);

  for (int i = 2; i < 30; i++)
    if (is_prime(i)) printf("%d ", i);
  printf("\n");

  // A linked list built on the stack.
  struct Node n3 = {3, 0};
  struct Node n2 = {2, &n3};
  struct Node n1 = {1, &n2};
  for (struct Node *p = &n1; p; p = p->next) printf("%d ", p->val);
  printf("\n");

  // Sieve of Eratosthenes.
  int sieve[50];
  for (int i = 0; i < 50; i++) sieve[i] = 1;
  sieve[0] = sieve[1] = 0;
  for (int i = 2; i * i < 50; i++)
    if (sieve[i])
      for (int j = i * i; j < 50; j += i) sieve[j] = 0;
  int count = 0;
  for (int i = 0; i < 50; i++) count += sieve[i];
  printf("%d primes under 50\n", count);

  // A tiny state machine over a string.
  char *text = "the quick brown fox";
  int words = 0, in_word = 0;
  for (char *p = text; *p; p++) {
    if (*p == ' ') in_word = 0;
    else if (!in_word) { in_word = 1; words++; }
  }
  printf("%d words\n", words);
  return 0;
}
